/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Filesystem device abstraction layer
 */

/*
 * [한국어 설명] SPDK Filesystem Device(fsdev) 공개 API 헤더 (fsdev.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK가 제공하는 "파일시스템 디바이스(fsdev)"의 공개(public) API를 선언한다.
 * fsdev는 SPDK의 또 다른 디바이스 추상화인 bdev(블록 디바이스)와 달리, 블록 단위 I/O가 아니라
 * POSIX 파일시스템 의미(파일/디렉토리/inode/extended attribute/lock 등)를 가진 디바이스를
 * 추상화한다. 즉 LUN/볼륨이 아니라 "마운트 가능한 파일시스템"에 가까운 객체를 노출한다.
 * 모든 API는 비동기(callback) 모델로 설계되어 있으며, 호출자는 spdk_io_channel 위에서 발급한
 * 후 cb_fn(cb_arg, ch, status, ...) 형태의 완료 콜백으로 결과를 수신한다. 이 헤더 자체에는
 * 구현이 없으며, 백엔드 모듈(예: lib/fsdev의 aiofs, passthru-fs 등)이 spdk_fsdev_fn_table을
 * 통해 실제 동작을 채워 넣는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 I/O 스택에서 fsdev는 bdev와 동등한 레벨의 "디바이스 추상화 레이어"에 위치한다. 주
 * 사용처는 vhost-user-fs / virtio-fs target이다 — QEMU 게스트가 호스트 디렉토리를 virtio-fs로
 * 마운트할 때, 게스트의 FUSE 요청이 vhost-user-fs target을 거쳐 이 헤더의 API를 호출하고,
 * 백엔드 모듈(예: aio 기반 호스트 디렉토리 노출)에서 실제 파일시스템 호출(openat, read,
 * writev 등)로 변환된다. 호출 체인은 다음과 같다:
 *
 *   [QEMU guest kernel: virtio-fs / FUSE]
 *     → [vhost-user-fs target (lib/vhost or virtio-fs target)]
 *     → [본 헤더: spdk_fsdev_lookup/read/write/...]
 *     → [fsdev 코어 (lib/fsdev/fsdev.c) — fn_table dispatch]
 *     → [fsdev 백엔드 모듈 (예: lib/fsdev/aio)]
 *     → [호스트 syscall (openat, preadv, pwritev, fsync, ...)]
 *
 * 실행 컨텍스트는 모두 SPDK 호스트 유저스페이스이며, 각 fsdev I/O는 발급된 spdk_thread
 * (=reactor)에 affinity가 고정되어 lockless로 처리된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈:
 *   - spdk/stdinc.h: POSIX 타입(uid_t, gid_t, mode_t, dev_t, off_t, iovec)
 *   - spdk/json.h: spdk_json_write_ctx (config dump용)
 *   - spdk/assert.h: SPDK_STATIC_ASSERT (구조체 ABI 사이즈 고정)
 *   - spdk/dma.h: spdk_memory_domain (zero-copy를 위한 메모리 도메인 추상화)
 *
 * 이 헤더에 의존하는 모듈:
 *   - lib/fsdev/*: fsdev 코어 구현이 이 API를 정의한다.
 *   - module/fsdev/aio/*: 호스트 디렉토리를 노출하는 AIO 기반 백엔드 모듈.
 *   - lib/vhost (vhost-user-fs target) 또는 별도 virtio-fs target: FUSE 요청을 fsdev API로
 *     변환하는 상위 계층.
 *
 * 데이터 흐름은 입력측에서 게스트의 virtio-fs descriptor → 호스트의 vhost target → 본
 * API → 백엔드 모듈 → 실제 파일시스템 → 결과는 cb_fn으로 역방향 흐름. 게스트의 페이지를
 * QEMU shared memory를 통해 zero-copy로 매핑하여 iovec에 담기는 점이 특징이다.
 *
 * 공유하는 핵심 자료구조:
 *   - struct spdk_fsdev: 디바이스 핸들(불투명)
 *   - struct spdk_fsdev_desc: open된 디바이스에 대한 디스크립터(불투명)
 *   - struct spdk_fsdev_file_object: FUSE의 nodeid에 대응하는 inode 식별자(불투명)
 *   - struct spdk_fsdev_file_handle: FUSE의 fh(file handle)에 대응하는 오픈 파일/디렉토리
 *     식별자(불투명)
 *   - struct spdk_fsdev_file_attr / file_statfs: stat / statfs 결과 구조체
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_fsdev_initialize / spdk_fsdev_finish: fsdev 서브시스템 초기화/종료(비동기)
 *   - spdk_fsdev_open / spdk_fsdev_close: 디바이스 디스크립터 획득/반납
 *   - spdk_fsdev_get_io_channel: 발급 thread에 바인딩되는 I/O 채널 획득
 *   - spdk_fsdev_mount / spdk_fsdev_umount: FUSE INIT/DESTROY에 대응. 마운트 시 옵션
 *     협상(writeback_cache_enabled, max_write 등)을 수행한다.
 *   - inode 작업: lookup / forget / getattr / setattr / readlink / symlink / mknod / mkdir /
 *     unlink / rmdir / rename / link
 *   - 파일 I/O: fopen / create / release / read / write / fsync / flush / fallocate
 *   - 디렉토리: opendir / readdir / releasedir / fsyncdir
 *   - xattr: getxattr / setxattr / listxattr / removexattr
 *   - 잠금: flock (현재 헤더는 BSD flock만 노출)
 *   - 통계/취소: statfs / abort
 *   - 고급 I/O: copy_file_range
 *
 *   주요 구조체:
 *   - spdk_fsdev_opts: fsdev 라이브러리 전역 옵션(IO pool 크기 등). opts_size로 ABI 호환.
 *   - spdk_fsdev_mount_opts: mount 시 협상되는 옵션(max_write, writeback_cache).
 *   - spdk_fsdev_io_opts: 개별 I/O의 메모리 도메인 등 확장 옵션.
 *   - spdk_fsdev_file_attr: getattr/setattr/readdir에서 반환되는 파일 메타데이터(size, mode,
 *     uid/gid, a/m/c-time, blocks, blksize, valid_ms 등).
 *   - spdk_fsdev_file_statfs: statfs 결과(blocks, bfree, bavail, files, ffree, bsize 등).
 *
 * 비동기 모델 / 스레드 affinity 요점:
 *   - 모든 fsdev_xxx() 함수는 0(요청 발급 성공) 또는 음수 errno(즉시 실패)를 반환한다.
 *     성공 반환 시 cb_fn이 반드시 한 번 호출된다(중간 실패 포함).
 *   - cb_fn은 ch가 속한 spdk_thread에서 호출된다 — 다른 스레드로의 메시지 전달 책임은 호출자가
 *     가진다.
 *   - I/O 채널을 획득한 스레드 외 다른 스레드에서 같은 채널로 fsdev_xxx()를 호출하는 것은
 *     불법이다(lockless 설계의 핵심).
 *   - unique 파라미터는 FUSE의 unique id에 직접 대응하며 abort 시 해당 I/O를 식별한다.
 *
 * 에러 코드 관례: 모든 함수의 즉시 반환값과 콜백 status 필드는 음수 errno이다(예: -ENOBUFS,
 * -ENOMEM, -EINVAL, -ENOENT 등). FUSE의 음수 errno 관례와 동일하다.
 */

#ifndef SPDK_FSDEV_H
/* [한국어] 이 헤더의 다중 포함을 막는 헤더 가드 시작.
 * 같은 컴파일 단위에서 fsdev.h가 여러 번 #include 되어도 선언이 중복되지 않도록 보호한다. */
#define SPDK_FSDEV_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 includes 묶음 — <stdint.h>, <stddef.h>, <sys/types.h> 등 POSIX 헤더를
 * 모아 놓은 SPDK 공용 헤더. 본 파일에서 사용하는 uint32_t, uint64_t, size_t, off_t,
 * uid_t, gid_t, mode_t, dev_t, struct iovec 등이 이를 통해 가시화된다. */
#include "spdk/json.h"
/* [한국어] SPDK JSON 라이브러리 — spdk_fsdev_subsystem_config_json()에서 사용하는
 * struct spdk_json_write_ctx 전방선언/사용을 위해 필요하다. RPC/configuration dump 경로에
 * 쓰인다. */
#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로 제공 — 본 파일에서 spdk_fsdev_opts,
 * spdk_fsdev_mount_opts, spdk_fsdev_io_opts의 sizeof를 ABI 안정성을 위해 컴파일 타임에
 * 검증한다(packed 구조체 + opts_size 패턴). */
#include "spdk/dma.h"
/* [한국어] SPDK DMA 추상화 — struct spdk_memory_domain 정의를 가져온다. fsdev IO가 게스트
 * 메모리 등 비호스트 메모리 영역의 페이로드를 다룰 때 사용된다(virtio-fs zero-copy 경로). */

#ifdef __cplusplus
/* [한국어] C++에서 본 헤더를 포함했을 때 함수 심볼이 C 링키지를 갖도록 extern "C" 블록을 연다.
 * SPDK 라이브러리는 C로 빌드되므로 C++ 사용자도 이 가드를 통해 링킹 호환을 얻는다. */
extern "C" {
#endif

/**
 * \brief SPDK filesystem device.
 *
 * This is a virtual representation of a filesystem device that is exported by the backend.
 */
/*
 * [한국어]
 * struct spdk_fsdev — 백엔드가 노출하는 파일시스템 디바이스의 가상 표현.
 *
 * 이 헤더에서는 전방선언만 노출되며 내부 필드는 fsdev 코어의 internal 헤더에서 정의된다.
 * 사용자는 이 포인터를 불투명 핸들로만 다루며, 디바이스 이름·메모리 도메인·이벤트 콜백 등은
 * spdk_fsdev_get_*() 접근자를 통해 조회한다. 한 SPDK 프로세스에 여러 fsdev이 등록될 수 있고,
 * 각 fsdev은 spdk_fsdev_open()으로 디스크립터를 얻은 뒤 spdk_fsdev_get_io_channel()로
 * thread-local I/O 채널을 만들어 사용한다.
 */
struct spdk_fsdev;

/** Asynchronous event type */
/*
 * [한국어]
 * enum spdk_fsdev_event_type — fsdev이 비동기로 호출자에게 통지하는 이벤트의 종류.
 *
 * spdk_fsdev_open() 시 등록한 spdk_fsdev_event_cb_t로 전달된다. 현재 정의된 이벤트는
 * REMOVE 하나뿐이며 향후 RESIZE, MEDIA_MGMT 등이 추가될 여지가 있다.
 */
enum spdk_fsdev_event_type {
	SPDK_FSDEV_EVENT_REMOVE,
	/* [한국어] fsdev이 시스템에서 제거(hot-remove)될 예정임을 디스크립터 보유자에게 통보한다.
	 * 설정자: fsdev 코어가 spdk_fsdev_unregister 또는 백엔드의 강제 제거 시 호출.
	 * 읽는 자: 사용자가 spdk_fsdev_open 시 등록한 이벤트 콜백.
	 * 의미: 콜백을 받은 측은 진행 중인 I/O 정리 후 spdk_fsdev_close()를 호출해야 한다.
	 *       close가 늦어지면 unregister 절차가 블록되므로 즉시 정리하는 것이 권장된다. */
};

/**
 * Filesystem device event callback.
 *
 * \param type Event type.
 * \param fsdev Filesystem device that triggered event.
 * \param event_ctx Context for the filesystem device event.
 */
/*
 * [한국어]
 * spdk_fsdev_event_cb_t — fsdev 비동기 이벤트 통지 콜백 타입(typedef).
 *
 * @param type: 이벤트 종류 (현재는 REMOVE만 정의).
 * @param fsdev: 이벤트를 발생시킨 디바이스 핸들. 사용자는 이 핸들로 자신이 보유한 디스크립터를
 *               역참조할 수 있다.
 * @param event_ctx: spdk_fsdev_open() 호출 시 사용자가 넘긴 컨텍스트 그대로 전달.
 *
 * 이 콜백은 spdk_fsdev_open()을 호출한 thread와 동일한 thread에서 호출됨이 보장된다(헤더의
 * spdk_fsdev_open doc 참조). 따라서 콜백 본문에서 spdk_fsdev_close 등 같은 thread 가정의
 * API를 직접 호출해도 안전하다.
 *
 * 호출 체인:
 *   fsdev 코어(unregister/hotremove) → [event_cb] → 사용자 정리 로직 → spdk_fsdev_close
 */
typedef void (*spdk_fsdev_event_cb_t)(enum spdk_fsdev_event_type type,
				      struct spdk_fsdev *fsdev,
				      void *event_ctx);

struct spdk_fsdev_fn_table;
/* [한국어] fsdev 백엔드 모듈이 채워야 하는 함수 포인터 테이블의 전방선언.
 * fsdev 코어는 사용자 API → fn_table 디스패치 → 백엔드 구현 순으로 호출을 라우팅한다.
 * 정의는 fsdev 코어의 internal 헤더(include/spdk_internal/fsdev_module.h 류)에 있다. */
struct spdk_io_channel;
/* [한국어] SPDK 공용 I/O 채널 핸들의 전방선언.
 * fsdev은 spdk_fsdev_get_io_channel()로 thread-local 채널을 만들어 모든 I/O API의 ch 인자에
 * 넘겨받는다. 채널은 호출 thread에 affinity가 고정된다(lockless 설계). */

/** fsdev status */
/*
 * [한국어]
 * enum spdk_fsdev_status — fsdev 코어가 디바이스의 수명주기 상태를 표현하기 위한 enum.
 * (이 헤더에서는 enum 정의만 노출되며, 실제 상태 전이는 fsdev 코어 내부에서 관리한다.)
 */
enum spdk_fsdev_status {
	SPDK_FSDEV_STATUS_INVALID,
	/* [한국어] 초기화되지 않았거나 이미 해체된 상태.
	 * 설정자: 구조체가 zero-init 되었을 때 또는 등록 해제 후. 읽는 자: fsdev 코어의 상태
	 * 검사 코드. 이 상태에서는 어떤 사용자 API 호출도 허용되지 않는다(EINVAL 반환). */
	SPDK_FSDEV_STATUS_READY,
	/* [한국어] 정상 등록되어 I/O를 받을 준비가 된 상태.
	 * 설정자: spdk_fsdev_register 성공 시 fsdev 코어가 설정.
	 * 읽는 자: 사용자 API의 진입 검사. spdk_fsdev_open 및 모든 I/O 발급은 READY일 때만
	 * 허용된다. */
	SPDK_FSDEV_STATUS_UNREGISTERING,
	/* [한국어] spdk_fsdev_unregister가 호출되어 해체 절차가 시작된 상태.
	 * 설정자: spdk_fsdev_unregister 진입 시. 읽는 자: 새로운 open/io 발급을 거부하는 검사.
	 * 모든 디스크립터가 close된 뒤 INVALID로 전이된다. */
	SPDK_FSDEV_STATUS_REMOVING,
	/* [한국어] hot-remove(외부 요청 또는 백엔드 통지)가 진행 중인 상태.
	 * 설정자: 백엔드가 디바이스 사라짐을 알리는 경로. 읽는 자: 이벤트 디스패치 코드.
	 * 이 상태에 들어가면 등록된 모든 event_cb가 SPDK_FSDEV_EVENT_REMOVE로 호출된다. */
};

/** fsdev library options */
/*
 * [한국어]
 * struct spdk_fsdev_opts — fsdev 라이브러리 전역(global) 옵션.
 *
 * 사용자가 spdk_fsdev_set_opts()로 설정하고 spdk_fsdev_get_opts()로 조회한다. ABI 호환성을
 * 위해 packed + opts_size 패턴을 사용한다 — 새 필드는 항상 구조체 끝에 추가하고,
 * 라이브러리는 사용자가 넘긴 opts_size까지만 읽고 나머지에 기본값을 채운다.
 */
struct spdk_fsdev_opts {
	/**
	 * The size of spdk_fsdev_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	uint32_t opts_size;
	/* [한국어] 호출자가 알고 있는 spdk_fsdev_opts의 sizeof를 담는 ABI 버저닝 필드.
	 * 설정자: 사용자 코드가 sizeof(struct spdk_fsdev_opts)를 직접 대입.
	 * 읽는 자: spdk_fsdev_set_opts/get_opts가 어디까지 안전하게 접근 가능한지 판단.
	 * 값 범위: 4 (opts_size 단독) ~ 현재 sizeof(struct spdk_fsdev_opts).
	 * 동기화: 옵션 설정은 보통 init 단계에서 단일 thread가 수행하므로 별도 락 불필요. */

	/**
	 * Size of fsdev IO objects pool
	 */
	uint32_t fsdev_io_pool_size;
	/* [한국어] 시스템 전역 spdk_fsdev_io 객체 풀의 크기(요청 동시 처리 가능 개수).
	 * 설정자: 사용자가 set_opts로 설정. 읽는 자: fsdev 코어의 풀 생성 코드.
	 * 값 범위: 양의 정수. 너무 작으면 I/O 발급 시 -ENOBUFS가 반환된다(헤더 곳곳의 \return 참조).
	 * 동기화: 풀 자체는 lockless ring(DPDK rte_mempool 등)으로 구현되므로 사용 시점에는
	 *         락이 필요 없다. */

	/**
	 * Size of fsdev IO objects cache per thread
	 */
	uint32_t fsdev_io_cache_size;
	/* [한국어] thread 별 spdk_fsdev_io 캐시 크기 — 전역 풀에서 미리 가져와 thread-local에
	 * 보관하는 fast-path 캐시.
	 * 설정자: 사용자가 set_opts로 설정. 읽는 자: io 채널 생성 시 thread별 캐시 할당.
	 * 값 범위: fsdev_io_pool_size 이하. 너무 크면 다른 thread의 starvation을 유발한다.
	 * 동기화: thread-local 자료구조이므로 락 없이 접근. */
} __attribute__((packed));
/* [한국어] packed 속성: ABI 호환을 위해 padding을 제거. 이 덕분에 다음 SPDK_STATIC_ASSERT가
 * 의도한 크기를 확정할 수 있다(아래 12바이트). */
SPDK_STATIC_ASSERT(sizeof(struct spdk_fsdev_opts) == 12, "Incorrect size");
/* [한국어] 컴파일 타임에 sizeof(spdk_fsdev_opts) == 12바이트(uint32_t × 3)를 강제.
 * 새 필드를 추가할 때 이 assert를 함께 갱신하지 않으면 빌드가 실패하여 ABI 호환을 의식하게
 * 만든다. */

/** fsdev mount options */
/*
 * [한국어]
 * struct spdk_fsdev_mount_opts — spdk_fsdev_mount() 호출 시 사용자와 백엔드 모듈 사이에
 * 협상되는 옵션 묶음. FUSE의 INIT 메시지 협상에 직접 대응한다(FUSE_CAP_*).
 *
 * 의미상의 IN/OUT 표시:
 *   - 일부 필드는 호출자가 채워서 넘기고(IN), 결과는 콜백의 opts로 받는다(OUT).
 *   - max_write는 OUT 전용(백엔드가 결정).
 *   - writeback_cache_enabled는 IN/OUT(요청 후 백엔드가 받아들이거나 거부).
 */
struct spdk_fsdev_mount_opts {
	/**
	 * The size of spdk_fsdev_mount_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	uint32_t opts_size;
	/* [한국어] mount_opts ABI 버저닝 필드(opts와 동일한 패턴).
	 * 설정자: 호출자. 읽는 자: fsdev 코어/백엔드. 값 범위: 4 ~ sizeof(현재 구조체).
	 * 동기화: mount 호출 동안만 유효한 단일 thread 컨텍스트. */

	/**
	 * OUT Maximum size of the write buffer
	 */
	uint32_t max_write;
	/* [한국어] OUT 전용. 백엔드가 한 번의 spdk_fsdev_write()에 허용하는 페이로드 최대 바이트.
	 * 설정자: mount cb_fn에서 opts로 채워져 콜백 호출자에게 반환됨(백엔드 결정).
	 * 읽는 자: virtio-fs target이 게스트 write 요청을 분할할 때 사용.
	 * 값 범위: 보통 1MiB~1GiB. FUSE의 max_write와 동일 의미.
	 * 동기화: mount 후 변경되지 않으므로 read-only 상수처럼 다룬다. */

	/**
	 * IN/OUT Indicates whether the writeback caching should be enabled.
	 *
	 * See FUSE I/O ([1]) doc for more info.
	 *
	 * [1] https://www.kernel.org/doc/Documentation/filesystems/fuse-io.txt
	 *
	 * This feature is disabled by default.
	 */
	uint8_t writeback_cache_enabled;
	/* [한국어] IN/OUT. writeback caching 활성화 협상값.
	 * 설정자(IN): 호출자가 0/1로 요청. 설정자(OUT): 백엔드가 수락 시 1, 거부 시 0으로 갱신.
	 * 의미: FUSE_CAP_WRITEBACK_CACHE에 대응 — 게스트 페이지 캐시가 dirty 페이지를 갖고
	 *       write-back 정책으로 디스패치한다. 일관성 가정이 달라지므로 백엔드가 거부할 수도
	 *       있다(예: 멀티-라이터 시 안전하지 않음).
	 * 값 범위: 0(disabled) | 1(enabled). 그 외 값은 미정의.
	 * 동기화: mount 시 협상되며 마운트 수명 동안 변경되지 않음. */

} __attribute__((packed));
/* [한국어] mount_opts 또한 ABI 호환을 위해 packed로 정렬 padding 제거. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_fsdev_mount_opts) == 9, "Incorrect size");
/* [한국어] sizeof(struct spdk_fsdev_mount_opts) == 9바이트 (4 + 4 + 1) 컴파일 타임 강제.
 * 새 필드 추가 시 이 값을 갱신해야 빌드가 통과한다. */

/**
 * Structure with optional fsdev IO parameters
 * The content of this structure must be valid until the IO is completed
 */
/*
 * [한국어]
 * struct spdk_fsdev_io_opts — 개별 read/write 호출에 부가하는 확장 옵션.
 *
 * 호출자가 NULL을 넘기면 기본 동작(호스트 메모리 사용)이 적용된다. 메모리 도메인을 지정하면
 * 백엔드는 직접 데이터 버퍼에 접근하지 않고 spdk_memory_domain API로 데이터 fetch/translate를
 * 수행한다 — virtio-fs의 게스트 메모리 zero-copy 경로에서 핵심.
 *
 * 수명: 구조체 자체와 가리키는 memory_domain은 cb_fn이 호출되어 IO가 완료될 때까지 호출자가
 * 살려둬야 한다. 그 사이에 free되면 use-after-free가 된다.
 */
struct spdk_fsdev_io_opts {
	/** Size of this structure in bytes */
	size_t size;
	/* [한국어] 호출자가 인지하는 본 구조체의 sizeof. ABI 호환 패턴.
	 * 설정자: 호출자. 읽는 자: fsdev 코어/백엔드의 옵션 파서.
	 * 값 범위: sizeof(struct spdk_fsdev_io_opts) (현재). 더 작은 값은 호환을 위해 허용되며
	 *         그 이후 필드는 기본값으로 처리된다.
	 * 동기화: io 발급~완료까지 변경 금지(immutable). */

	/** Memory domain which describes payload in this IO. fsdev must support DMA device type that
	 * can access this memory domain, refer to \ref spdk_fsdev_get_memory_domains and
	 * \ref spdk_memory_domain_get_dma_device_type
	 * If set, that means that data buffers can't be accessed directly and the memory domain must
	 * be used to fetch data to local buffers or to translate data to another memory domain */
	struct spdk_memory_domain *memory_domain;
	/* [한국어] 페이로드(iovec)의 메모리 도메인. NULL이면 일반 호스트 메모리.
	 * 설정자: 호출자(virtio-fs target 등). 읽는 자: 백엔드 모듈 — 직접 접근 불가 시
	 *         spdk_memory_domain_translate_data/pull_data 등을 사용해 호스트 버퍼로 가져와야 함.
	 * 값 범위: NULL 또는 spdk_fsdev_get_memory_domains에서 보고된 도메인 중 하나.
	 *         지원되지 않는 도메인을 넘기면 백엔드가 -ENOTSUP를 반환할 수 있다.
	 * 동기화: 메모리 도메인 자체는 RPC/관리 thread에서 등록되며, IO 발급 thread에서는
	 *         read-only 핸들로 사용한다. */

	/** Context to be passed to memory domain operations */
	void *memory_domain_ctx;
	/* [한국어] 메모리 도메인 콜백에 그대로 전달되는 호출자 컨텍스트(불투명).
	 * 설정자: 호출자. 읽는 자: 백엔드가 spdk_memory_domain_*() 호출 시 ctx 인자로 전달.
	 * 값 범위: 임의 포인터(NULL 허용). 의미는 memory_domain의 정의에 따른다.
	 * 동기화: 호출자가 IO 수명동안 보장. */
} __attribute__((packed));
/* [한국어] io_opts도 packed — size + 두 포인터의 정렬 padding 제거. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_fsdev_io_opts) == 24, "Incorrect size");
/* [한국어] 64-bit 환경 기준 sizeof = 8(size_t) + 8(ptr) + 8(ptr) = 24. 컴파일 타임 검증. */

/**
 * \brief Handle to an opened SPDK filesystem device.
 */
/*
 * [한국어]
 * struct spdk_fsdev_desc — spdk_fsdev_open()이 반환하는 오픈 디스크립터(불투명).
 *
 * fsdev 핸들과 사용자 컨텍스트(이벤트 콜백, 채널 트래커 등)를 묶은 객체. close까지 살아있어야
 * 하며, open한 thread와 동일한 thread에서 close 되어야 한다.
 */
struct spdk_fsdev_desc;

/**
 * Filesystem device initialization callback.
 *
 * \param cb_arg Callback argument.
 * \param rc 0 if filesystem device initialized successfully or negative errno if it failed.
 */
/*
 * [한국어]
 * spdk_fsdev_init_cb — spdk_fsdev_initialize() 완료 통지 콜백.
 *
 * @param cb_arg: initialize 호출 시 사용자가 넘긴 컨텍스트(그대로 전달).
 * @param rc: 0이면 fsdev 서브시스템(코어 + 등록된 모든 백엔드 모듈) 초기화 성공.
 *            음수면 errno로, 호출자는 spdk_app 종료/롤백을 수행해야 한다.
 *
 * 호출 컨텍스트: spdk_fsdev_initialize()를 호출한 thread에서 호출됨이 일반적이지만,
 * 백엔드의 비동기 초기화 경로에 따라 다른 thread일 수도 있다 — 구현에 의존하지 말고
 * spdk_thread_send_msg로 자기 thread에 다시 게시하는 것이 안전하다.
 *
 * 호출 체인:
 *   spdk_subsystem_init → spdk_fsdev_initialize → 모든 백엔드 init 완료 → [init_cb]
 */
typedef void (*spdk_fsdev_init_cb)(void *cb_arg, int rc);

/**
 * Filesystem device finish callback.
 *
 * \param cb_arg Callback argument.
 */
/*
 * [한국어]
 * spdk_fsdev_fini_cb — spdk_fsdev_finish() 완료 통지 콜백.
 *
 * @param cb_arg: 호출자가 넘긴 컨텍스트.
 *
 * 모든 등록 fsdev이 unregister되고 백엔드 모듈이 정리되었을 때 호출된다. 이후 fsdev API
 * 호출은 모두 무효(undefined). 일반적으로 spdk_app 종료의 마지막 단계에서 호출된다.
 *
 * 호출 체인:
 *   spdk_app_stop → spdk_fsdev_finish → 모든 unregister 완료 → [fini_cb]
 */
typedef void (*spdk_fsdev_fini_cb)(void *cb_arg);

/**
 * Initialize filesystem device modules.
 *
 * \param cb_fn Called when the initialization is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_fsdev_initialize - fsdev 서브시스템 및 등록된 모든 백엔드 모듈을 비동기 초기화한다.
 *
 * @cb_fn: 초기화 완료 시 호출될 콜백.
 * @cb_arg: cb_fn의 첫 인자로 전달될 사용자 컨텍스트.
 *
 * 호출 시점: spdk_app_start 이후, 임의 fsdev 사용 전. 보통 SPDK subsystem framework가 자동 호출.
 * 동작:
 *   1) 전역 풀(spdk_fsdev_io 풀)을 spdk_fsdev_opts 기준으로 생성.
 *   2) 등록된 fsdev 모듈 목록을 순회하며 각 모듈의 module_init()을 비동기 호출.
 *   3) 모두 완료되면 cb_fn(cb_arg, 0) 호출. 하나라도 실패 시 cb_fn(cb_arg, -errno).
 * 실행 컨텍스트: 보통 init thread (메인 spdk_thread). cross-thread 발급 시 lockless 메시지로
 * 제출되어야 한다.
 *
 * 호출 체인:
 *   spdk_subsystem_init_next("fsdev") → [spdk_fsdev_initialize] → 각 모듈 module_init
 */
void spdk_fsdev_initialize(spdk_fsdev_init_cb cb_fn, void *cb_arg);

/**
 * Perform cleanup work to remove the registered filesystem device modules.
 *
 * \param cb_fn Called when the removal is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
/*
 * [한국어]
 * spdk_fsdev_finish - fsdev 서브시스템을 정리/해체한다(모든 등록 모듈 unregister).
 *
 * @cb_fn: 정리 완료 콜백. 모든 fsdev이 unregister되고 풀이 free된 뒤 호출.
 * @cb_arg: cb_fn에 전달될 사용자 컨텍스트.
 *
 * 호출 시점: spdk_app_stop 직전. 이 함수 호출 후에는 어떤 fsdev API도 호출하면 안 된다.
 * 동작: 등록된 모든 fsdev에 unregister를 트리거하고, 모듈 module_fini를 호출하며, 전역
 * spdk_fsdev_io 풀을 해제한다.
 *
 * 호출 체인:
 *   spdk_app_stop → spdk_subsystem_fini → [spdk_fsdev_finish] → 각 모듈 module_fini
 */
void spdk_fsdev_finish(spdk_fsdev_fini_cb cb_fn, void *cb_arg);

/**
 * Get the full configuration options for the registered filesystem device modules and created fsdevs.
 *
 * \param w pointer to a JSON write context where the configuration will be written.
 */
/*
 * [한국어]
 * spdk_fsdev_subsystem_config_json - 현재 fsdev 구성(모듈/디바이스 옵션 등)을 JSON으로 직렬화.
 *
 * @w: 결과를 기록할 JSON write 컨텍스트(spdk/json.h).
 *
 * 호출 시점: SPDK가 "save_config" RPC를 처리할 때 fsdev subsystem이 호출되어 자신의 구성을
 * 출력. 결과 JSON은 다음 부팅 시 동일한 fsdev/모듈을 자동으로 재구성하는 용도로 쓰인다.
 * 동작: 각 모듈의 dump_info_json 콜백을 호출하고, 등록된 fsdev 마다 메타데이터를 기록.
 *
 * 호출 체인:
 *   RPC "save_config" → spdk_subsystem_config → [spdk_fsdev_subsystem_config_json]
 */
void spdk_fsdev_subsystem_config_json(struct spdk_json_write_ctx *w);

/**
 * Get filesystem device module name.
 *
 * \param fsdev Filesystem device to query.
 * \return Name of fsdev module as a null-terminated string.
 */
/*
 * [한국어]
 * spdk_fsdev_get_module_name - fsdev을 노출하는 백엔드 모듈의 이름을 반환.
 *
 * @fsdev: 조회 대상 fsdev 핸들.
 * @return: NUL-종결 문자열(예: "aio", "passthru-fs"). 모듈이 살아 있는 동안 유효.
 *
 * 사용처: 진단/RPC dump. 호출자는 반환된 문자열을 free하면 안 된다. */
const char *spdk_fsdev_get_module_name(const struct spdk_fsdev *fsdev);

/**
 * Open a filesystem device for I/O operations.
 *
 * \param fsdev_name Filesystem device name to open.
 * \param event_cb notification callback to be called when the fsdev triggers
 * asynchronous event such as fsdev removal. This will always be called on the
 * same thread that spdk_fsdev_open() was called on. In case of removal event
 * the descriptor will have to be manually closed to make the fsdev unregister
 * proceed.
 * \param event_ctx param for event_cb.
 * \param desc output parameter for the descriptor when operation is successful
 * \return 0 if operation is successful, suitable errno value otherwise
 */
/*
 * [한국어]
 * spdk_fsdev_open - 등록된 fsdev에 디스크립터를 열고 이벤트 콜백을 구독한다.
 *
 * @fsdev_name: spdk_fsdev_register 시 부여된 이름. 대소문자 구분.
 * @event_cb: hot-remove 등 비동기 이벤트 통지 콜백. 같은 thread에서만 호출됨이 보장.
 * @event_ctx: event_cb에 전달될 사용자 컨텍스트.
 * @desc: [OUT] 성공 시 채워질 디스크립터 포인터의 주소.
 * @return: 0(성공) 또는 음수 errno(예: -ENODEV 이름 없음, -ENOMEM 풀 부족, -EBUSY 등록 미완료).
 *
 * 동작: fsdev 코어가 이름으로 등록 테이블을 검색하고, 새로운 spdk_fsdev_desc를 alloc하여
 * 이벤트 구독 리스트에 추가한 뒤 *desc에 반환.
 * 실행 컨텍스트: 임의 SPDK thread. 단, 이후 close/get_io_channel/event_cb는 모두 이 thread를
 * 가정한다(thread affinity).
 *
 * 호출 체인:
 *   사용자 → [spdk_fsdev_open] → fsdev 코어의 lookup → desc 등록
 */
int spdk_fsdev_open(const char *fsdev_name, spdk_fsdev_event_cb_t event_cb,
		    void *event_ctx, struct spdk_fsdev_desc **desc);

/**
 * Close a previously opened filesystem device.
 *
 * Must be called on the same thread that the spdk_fsdev_open()
 * was performed on.
 *
 * \param desc Filesystem device descriptor to close.
 */
/*
 * [한국어]
 * spdk_fsdev_close - 디스크립터를 반납한다(이벤트 구독 해제, 자원 해제).
 *
 * @desc: 닫을 디스크립터(이전 spdk_fsdev_open으로 획득한 것).
 *
 * 동작: 이벤트 콜백 구독을 끊고 desc를 free. unregister 진행 중이라면 마지막 desc close가
 * 실제 unregister 절차를 풀어주는 트리거 역할을 한다.
 * 실행 컨텍스트: spdk_fsdev_open과 동일 thread여야 한다(헤더 doc 참조). 다른 thread에서
 * 닫고 싶다면 spdk_thread_send_msg로 원래 thread에 위임해야 한다.
 *
 * 호출 체인:
 *   사용자 또는 event_cb(REMOVE) → [spdk_fsdev_close] → fsdev 코어 정리
 */
void spdk_fsdev_close(struct spdk_fsdev_desc *desc);

/**
 * Get filesystem device name.
 *
 * \param fsdev filesystem device to query.
 * \return Name of fsdev as a null-terminated string.
 */
/*
 * [한국어]
 * spdk_fsdev_get_name - fsdev의 등록 이름을 반환.
 *
 * @fsdev: 조회 대상 핸들.
 * @return: NUL-종결 문자열. fsdev 수명동안 유효. 호출자가 free 금지.
 */
const char *spdk_fsdev_get_name(const struct spdk_fsdev *fsdev);

/**
 * Get the fsdev associated with a fsdev descriptor.
 *
 * \param desc Open filesystem device descriptor
 * \return fsdev associated with the descriptor
 */
/*
 * [한국어]
 * spdk_fsdev_desc_get_fsdev - 디스크립터로부터 연결된 fsdev 핸들을 얻는다.
 *
 * @desc: 오픈된 디스크립터.
 * @return: 디스크립터가 가리키는 fsdev. NULL일 수 없음(close 전이라면).
 *
 * 사용처: 사용자가 desc만 들고 있는 경로에서 fsdev 메타정보(name, module, memory_domain)를
 * 조회할 때.
 */
struct spdk_fsdev *spdk_fsdev_desc_get_fsdev(struct spdk_fsdev_desc *desc);

/**
 * Obtain an I/O channel for the filesystem device opened by the specified
 * descriptor. I/O channels are bound to threads, so the resulting I/O
 * channel may only be used from the thread it was originally obtained
 * from.
 *
 * \param desc Filesystem device descriptor.
 *
 * \return A handle to the I/O channel or NULL on failure.
 */
/*
 * [한국어]
 * spdk_fsdev_get_io_channel - 호출 thread에 바인딩되는 I/O 채널을 획득.
 *
 * @desc: 오픈된 디스크립터.
 * @return: I/O 채널 핸들 또는 실패 시 NULL.
 *
 * 동작: 호출 thread에 thread-local IO 채널이 없으면 새로 만들고 reference count를 증가, 있으면
 * 그것을 그대로 반환(refcount 증가). 실제 자원 해제는 spdk_put_io_channel()로 마지막 ref가
 * 풀릴 때 일어난다.
 * 실행 컨텍스트: 임의 SPDK thread. 반환된 채널은 그 thread에서만 사용해야 한다(lockless).
 *
 * 호출 체인:
 *   사용자(IO 발급 전) → [spdk_fsdev_get_io_channel] → spdk_get_io_channel(코어) → 새 채널 또는 캐시
 */
struct spdk_io_channel *spdk_fsdev_get_io_channel(struct spdk_fsdev_desc *desc);

/**
 * Set the options for the fsdev library.
 *
 * \param opts options to set
 * \return 0 on success.
 * \return -EINVAL if the options are invalid.
 */
/*
 * [한국어]
 * spdk_fsdev_set_opts - fsdev 라이브러리 전역 옵션을 적용.
 *
 * @opts: 사용자가 채운 옵션 구조체. opts_size로 ABI 호환 처리.
 * @return: 0 성공, -EINVAL 옵션 검증 실패(예: pool < cache).
 *
 * 호출 시점: spdk_fsdev_initialize 이전. initialize 후 호출 시 효과 미정.
 */
int spdk_fsdev_set_opts(const struct spdk_fsdev_opts *opts);

/**
 * Get the options for the fsdev library.
 *
 * \param opts Output parameter for options.
 * \param opts_size sizeof(*opts)
 */
/*
 * [한국어]
 * spdk_fsdev_get_opts - 현재 적용된 fsdev 라이브러리 옵션을 조회.
 *
 * @opts: [OUT] 호출자가 할당한 버퍼.
 * @opts_size: 호출자가 알고 있는 sizeof(*opts) (ABI 호환).
 * @return: 0 성공, -EINVAL 잘못된 인자.
 *
 * 라이브러리는 opts_size까지만 채워주므로 더 큰 버전이 컴파일된 호출자도 안전하게 동작한다.
 */
int spdk_fsdev_get_opts(struct spdk_fsdev_opts *opts, size_t opts_size);

/**
 * Get SPDK memory domains used by the given fsdev. If fsdev reports that it uses memory domains
 * that means that it can work with data buffers located in those memory domains.
 *
 * The user can call this function with \b domains set to NULL and \b array_size set to 0 to get the
 * number of memory domains used by fsdev
 *
 * \param fsdev filesystem device
 * \param domains pointer to an array of memory domains to be filled by this function. The user should allocate big enough
 * array to keep all memory domains used by fsdev and all underlying fsdevs
 * \param array_size size of \b domains array
 * \return the number of entries in \b domains array or negated errno. If returned value is bigger than \b array_size passed by the user
 * then the user should increase the size of \b domains array and call this function again. There is no guarantees that
 * the content of \b domains array is valid in that case.
 *         -EINVAL if input parameters were invalid
 */
/*
 * [한국어]
 * spdk_fsdev_get_memory_domains - 본 fsdev이 지원하는 메모리 도메인 목록을 조회.
 *
 * @fsdev: 조회 대상.
 * @domains: [OUT] 호출자가 할당한 배열. NULL이면 카운트만 얻고 싶다는 의미.
 * @array_size: domains 배열의 엔트리 수.
 * @return: 채울 수 있는 엔트리 수(>= 0) 또는 -EINVAL.
 *          반환값 > array_size이면 배열이 너무 작음 → 더 크게 잡아 재호출.
 *
 * 사용처: virtio-fs target이 게스트 메모리를 직접 도메인으로 등록 가능한지 결정. 백엔드가
 * RDMA, GPU, custom DMA 도메인을 지원하면 그 목록이 노출된다.
 *
 * 호출 체인:
 *   상위(virtio-fs target 초기화) → [spdk_fsdev_get_memory_domains] → fn_table->get_memory_domains
 */
int spdk_fsdev_get_memory_domains(struct spdk_fsdev *fsdev, struct spdk_memory_domain **domains,
				  int array_size);


/* 'to_set' flags in spdk_fsdev_setattr */
/* [한국어] 아래 매크로는 spdk_fsdev_setattr()의 to_set 비트 마스크를 정의한다.
 * FUSE의 FATTR_* 플래그와 의미가 동일하며, 클라이언트는 변경하려는 필드만 비트를 켜서 넘긴다.
 * 단순 조합 가능(OR). 플래그가 켜진 필드만 attr 구조체에서 읽혀 적용된다. */
#define FSDEV_SET_ATTR_MODE	(1 << 0)
/* [한국어] mode(권한 비트 + 파일 타입) 변경. attr.mode를 적용. chmod(2)와 동등. */
#define FSDEV_SET_ATTR_UID	(1 << 1)
/* [한국어] uid 변경. chown(2)의 uid 부분. */
#define FSDEV_SET_ATTR_GID	(1 << 2)
/* [한국어] gid 변경. chown(2)의 gid 부분. */
#define FSDEV_SET_ATTR_SIZE	(1 << 3)
/* [한국어] 파일 크기 변경(truncate). attr.size를 적용. truncate(2)/ftruncate(2)에 대응. */
#define FSDEV_SET_ATTR_ATIME	(1 << 4)
/* [한국어] access time을 attr.atime/atimensec 값으로 갱신. utimensat(2)와 동등. */
#define FSDEV_SET_ATTR_MTIME	(1 << 5)
/* [한국어] modification time을 attr.mtime/mtimensec 값으로 갱신. */
#define FSDEV_SET_ATTR_ATIME_NOW	(1 << 6)
/* [한국어] access time을 현재 시각으로(UTIME_NOW). attr.atime은 무시. */
#define FSDEV_SET_ATTR_MTIME_NOW	(1 << 7)
/* [한국어] modification time을 현재 시각으로(UTIME_NOW). attr.mtime은 무시. */
#define FSDEV_SET_ATTR_CTIME	(1 << 8)
/* [한국어] change time(메타데이터 변경 시각) 갱신. POSIX 표준 utimensat은 ctime 직접 변경을
 * 지원하지 않지만 fsdev은 백엔드가 허용 시 강제 셋팅을 표현한다. */

struct spdk_fsdev_file_object;
/* [한국어] 파일/디렉토리/심볼릭링크 등 모든 inode를 식별하는 불투명 핸들의 전방선언.
 * FUSE 프로토콜의 nodeid(uint64_t)에 대응. lookup이 부여하고 forget이 참조 카운트를 줄인다.
 * 같은 파일에 대한 여러 lookup은 같은 fobject를 가리킬 수도 있고 다른 인스턴스일 수도 있다 —
 * 백엔드 구현에 의존. 실제 메모리 표현은 백엔드 모듈이 정의한다. */
struct spdk_fsdev_file_handle;
/* [한국어] 오픈된 파일/디렉토리의 fh(file handle) 불투명 핸들 전방선언.
 * FUSE의 fh와 1:1 대응 — fopen/create/opendir이 발급, release/releasedir이 회수.
 * 한 inode(fobject)에 여러 fhandle이 동시에 존재할 수 있다(O_RDONLY 두 개 등). */

/*
 * [한국어]
 * struct spdk_fsdev_file_attr - 파일/디렉토리 메타데이터(stat 결과 + FUSE 전용 valid_ms).
 *
 * 채워지는 시점: lookup, getattr, setattr, readdirplus 류 콜백의 attr 인자로 전달된다.
 * 사용자(virtio-fs target)는 이 값을 게스트의 struct stat으로 변환해 응답한다.
 * 시간 필드는 초(uint64_t) + 나노초(uint32_t)로 분할되어 있다(POSIX timespec 양식).
 */
struct spdk_fsdev_file_attr {
	uint64_t ino;
	/* [한국어] inode 번호. 게스트가 보는 stat.st_ino에 대응.
	 * 설정자: 백엔드(호스트의 실제 inode 또는 가상 매핑값). 읽는 자: virtio-fs target의 응답
	 * 직렬화. 값 범위: 0은 보통 미정의. 동일 fsdev 내에서 안정적이어야 한다. */
	uint64_t size;
	/* [한국어] 파일 바이트 크기(stat.st_size). 디렉토리는 의미가 백엔드 마다 다를 수 있다.
	 * 설정자: 백엔드. 읽는 자: 게스트. 단위: 바이트. */
	uint64_t blocks;
	/* [한국어] 할당된 512바이트 블록 수(stat.st_blocks). 희소 파일에서는 size보다 작을 수 있다.
	 * 설정자: 백엔드. 단위: 512B 블록. */
	uint64_t atime;
	/* [한국어] 최근 접근 시각의 epoch 초. atimensec과 함께 timespec을 구성. */
	uint64_t mtime;
	/* [한국어] 최근 데이터 수정 시각의 epoch 초. mtimensec과 함께. */
	uint64_t ctime;
	/* [한국어] 최근 메타데이터 변경 시각의 epoch 초. ctimensec과 함께. */
	uint32_t atimensec;
	/* [한국어] atime의 나노초 부분 [0, 999_999_999]. */
	uint32_t mtimensec;
	/* [한국어] mtime의 나노초 부분 [0, 999_999_999]. */
	uint32_t ctimensec;
	/* [한국어] ctime의 나노초 부분 [0, 999_999_999]. */
	uint32_t mode;
	/* [한국어] 파일 타입(S_IFREG/S_IFDIR/...) + 권한(0-12 비트). chmod 결과가 반영된다.
	 * 설정자: 백엔드. 읽는 자: 게스트. 매크로: <sys/stat.h>의 S_IF*, S_I[RWX]*. */
	uint32_t nlink;
	/* [한국어] 하드 링크 수(stat.st_nlink). 디렉토리는 자식 + 자기 자신("."), 부모(".."). */
	uint32_t uid;
	/* [한국어] 소유자 UID. 백엔드는 호스트 UID 또는 매핑된 값을 반환. */
	uint32_t gid;
	/* [한국어] 소유자 GID. uid와 동일한 매핑 정책. */
	uint32_t rdev;
	/* [한국어] 디바이스 노드일 때(major,minor) 인코딩. 일반 파일/디렉토리에선 0. */
	uint32_t blksize;
	/* [한국어] 파일시스템이 권장하는 블록 크기(stat.st_blksize). I/O 최적 단위 힌트. */
	uint32_t valid_ms;
	/* [한국어] 이 attr가 유효한 시간(밀리초). 게스트의 attr/dentry 캐시 timeout으로 전파.
	 * 설정자: 백엔드 — 0이면 매번 재검증, 큰 값이면 캐시를 적극 사용.
	 * 읽는 자: virtio-fs target이 FUSE의 attr_timeout/entry_timeout 필드로 변환.
	 * 의미상 한계: writeback_cache 활성화 시 일관성을 해칠 수 있으니 백엔드가 적절히 결정. */
};

/*
 * [한국어]
 * struct spdk_fsdev_file_statfs - statfs(2) 결과 구조체. 마운트 단위 통계.
 */
struct spdk_fsdev_file_statfs {
	uint64_t blocks;
	/* [한국어] 총 블록 수(frsize 단위). */
	uint64_t bfree;
	/* [한국어] 비어 있는 블록 수(루트만 사용 가능 포함). */
	uint64_t bavail;
	/* [한국어] 일반 사용자가 사용 가능한 블록 수(quota/예약 제외). */
	uint64_t files;
	/* [한국어] 총 inode 수. */
	uint64_t ffree;
	/* [한국어] 비어 있는 inode 수. */
	uint32_t bsize;
	/* [한국어] 파일시스템의 추천 I/O 블록 크기(stat.st_blksize와 유사). */
	uint32_t namelen;
	/* [한국어] 파일 이름 최대 바이트 수. */
	uint32_t frsize;
	/* [한국어] fragment(논리 블록) 크기. blocks/bfree/bavail의 단위. */
};

/**
 * Mount operation completion callback.
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status Operation status, 0 on success or error code otherwise.
 * \param opts Result options.
 * \param root_fobject Root file object
 */
/*
 * [한국어]
 * spdk_fsdev_mount_cpl_cb - mount 완료 콜백.
 *
 * @cb_arg: 호출자 컨텍스트.
 * @ch: 발급에 사용한 io channel.
 * @status: 0 성공 / 음수 errno.
 * @opts: 협상 결과 옵션(IN/OUT). max_write가 채워지고 writeback_cache_enabled가 갱신될 수 있음.
 * @root_fobject: 루트 디렉토리(/)에 대한 fobject — 이후 lookup의 첫 parent로 사용된다.
 *
 * 호출 체인:
 *   spdk_fsdev_mount → 백엔드 mount 핸들러 → [mount_cpl_cb] → 호출자가 root_fobject 저장
 */
typedef void (spdk_fsdev_mount_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				       const struct spdk_fsdev_mount_opts *opts,
				       struct spdk_fsdev_file_object *root_fobject);

/**
 * Mount the filesystem.
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param opts Requested options.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *
 * Note: the \p opts are the subject of negotiation. An API user provides a desired \p opts here
 * and gets a result \p opts in the \p cb_fn. The result \p opts are filled by the underlying
 * fsdev module which may agree or reduce (but not expand) the desired features set.
 */
/*
 * [한국어]
 * spdk_fsdev_mount - 파일시스템을 마운트하고 루트 fobject를 획득한다(FUSE INIT 대응).
 *
 * @desc: 디스크립터.
 * @ch: 호출 thread 바운드 io channel.
 * @unique: FUSE unique id (abort 시 식별자).
 * @opts: [IN] 요청 옵션. cb_fn에서 OUT으로 협상 결과를 받는다.
 * @cb_fn: 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0(콜백이 반드시 한 번 호출됨), 음수 errno(콜백 미호출).
 *
 * 동작: fsdev 코어가 fn_table->mount를 호출, 백엔드가 옵션을 검토하고 환원(features 축소)하여
 * 콜백으로 회신. 협상은 한 번만 일어나며 이후 spdk_fsdev_umount까지 변경 불가.
 *
 * 호출 체인:
 *   virtio-fs target(FUSE INIT 수신) → [spdk_fsdev_mount] → 백엔드 mount
 */
int spdk_fsdev_mount(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		     uint64_t unique, const struct spdk_fsdev_mount_opts *opts,
		     spdk_fsdev_mount_cpl_cb cb_fn, void *cb_arg);

/**
 * Umount operation completion callback.
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 */
/*
 * [한국어]
 * spdk_fsdev_umount_cpl_cb - umount 완료 콜백.
 *
 * @cb_arg: 호출자 컨텍스트.
 * @ch: 사용된 io channel.
 *
 * 주의: status 인자가 없다 — umount는 에러 보고가 의미 없는 셧다운 경로로 설계됨. */
typedef void (spdk_fsdev_umount_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch);

/**
 * Unmount the filesystem.
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *
 * NOTE: on unmount the lookup count for all fobjects implicitly drops to zero.
 */
/*
 * [한국어]
 * spdk_fsdev_umount - 파일시스템을 언마운트(FUSE DESTROY 대응).
 *
 * @desc: 디스크립터.
 * @ch: io channel.
 * @unique: FUSE unique id.
 * @cb_fn / @cb_arg: 완료 콜백 / 컨텍스트.
 * @return: 0 또는 -errno.
 *
 * 부수 효과: 모든 fobject의 lookup count가 0으로 강제(forget을 일일이 보낼 필요 없음).
 * 이후 동일 fobject 사용은 모두 ENOENT 또는 백엔드별 무효 응답.
 */
int spdk_fsdev_umount(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		      uint64_t unique, spdk_fsdev_umount_cpl_cb cb_fn, void *cb_arg);

/**
 * Lookup file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fobject File object.
 * \param attr File attributes.
 */
/*
 * [한국어]
 * spdk_fsdev_lookup_cpl_cb - lookup 완료 콜백.
 *
 * @cb_arg/@ch: 표준.
 * @status: 0 성공 시 fobject/attr 유효. 보통 -ENOENT(이름 없음), -EACCES, -ENAMETOOLONG.
 * @fobject: 발견된 entry의 inode 핸들. lookup count가 1 증가된 상태로 반환되며, 추후
 *           spdk_fsdev_forget(nlookup=1)으로 균형을 맞춰야 한다.
 * @attr: entry의 메타데이터 — valid_ms를 함께 봐야 캐시 만료를 결정 가능.
 */
typedef void (spdk_fsdev_lookup_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
					struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr);

/**
 * Look up a directory entry by name and get its attributes
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory. NULL for the root directory.
 * \param name The name to look up. Ignored if parent_fobject is NULL.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_lookup - 디렉토리에서 이름으로 entry를 찾고 attr를 받아온다(FUSE LOOKUP).
 *
 * @desc: 디스크립터.
 * @ch: io channel.
 * @unique: FUSE unique id(abort 식별자).
 * @parent_fobject: 부모 디렉토리. NULL이면 root를 의미하며 이때 name은 무시(루트 자체 attr 조회).
 * @name: NUL-종결 entry 이름. parent_fobject != NULL일 때만 사용.
 * @cb_fn/@cb_arg: 완료 콜백/컨텍스트.
 * @return: 0(콜백 호출됨) 또는 음수 errno(미호출). 즉시 실패 케이스: -ENOBUFS(IO 풀 고갈),
 *          -ENOMEM(보조 메모리 부족).
 *
 * 의미: 성공 반환된 fobject는 lookup count가 1 증가한 상태이므로, 사용자는 자료구조에서
 * fobject를 폐기할 때 spdk_fsdev_forget(nlookup=N)으로 카운트를 정확히 회수해야 한다(누수 방지).
 *
 * 호출 체인:
 *   FUSE_LOOKUP 수신 → [spdk_fsdev_lookup] → 백엔드 lookup → [lookup_cpl_cb]
 */
int spdk_fsdev_lookup(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *parent_fobject, const char *name,
		      spdk_fsdev_lookup_cpl_cb cb_fn, void *cb_arg);

/**
 * Look up file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status Operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_forget_cpl_cb - forget 완료 콜백.
 *
 * @cb_arg/@ch/@status: 표준. status는 보통 0(성공) 또는 -EINVAL(잘못된 fobject).
 * 의미: forget은 본질적으로 fire-and-forget이지만 SPDK는 일관성을 위해 status를 노출한다.
 */
typedef void (spdk_fsdev_forget_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Remove file object from internal cache
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param nlookup Number of lookups to forget.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_forget - fobject의 lookup 참조 카운트를 nlookup만큼 감소(FUSE FORGET 대응).
 *
 * @desc/@ch/@unique: 표준.
 * @fobject: forget 대상.
 * @nlookup: 회수할 lookup 횟수. 보통 게스트 측 dentry/inode가 evict될 때 누적치를 한 번에 보낸다.
 * @cb_fn/@cb_arg: 완료 콜백/컨텍스트.
 * @return: 0 또는 -errno.
 *
 * 의미: 카운트가 0이 되면 백엔드는 fobject 자원(open file handle 캐시 등)을 자유롭게 해제할 수 있다.
 * 호출자는 forget 발급 후 해당 fobject를 더 이상 사용하면 안 된다(use-after-free 위험).
 */
int spdk_fsdev_forget(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *fobject, uint64_t nlookup,
		      spdk_fsdev_forget_cpl_cb cb_fn, void *cb_arg);

/**
 * Read symbolic link operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status Operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param linkname symbolic link contents
 */
/*
 * [한국어]
 * spdk_fsdev_readlink_cpl_cb - readlink 완료 콜백.
 *
 * @linkname: NUL-종결 심볼릭 링크 타겟 경로. 콜백 반환 후 무효(스택/임시 버퍼).
 */
typedef void (spdk_fsdev_readlink_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		const char *linkname);

/**
 * Read symbolic link
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_readlink - 심볼릭 링크의 타겟 경로를 읽는다(FUSE READLINK).
 *
 * @desc/@ch/@unique: 표준.
 * @fobject: 심볼릭 링크 자체의 inode.
 * @cb_fn/@cb_arg: 완료 콜백/컨텍스트.
 * @return: 0 또는 음수 errno.
 *
 * fobject가 심볼릭 링크가 아니면 백엔드가 -EINVAL 반환.
 */
int spdk_fsdev_readlink(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t unique, struct spdk_fsdev_file_object *fobject,
			spdk_fsdev_readlink_cpl_cb cb_fn, void *cb_arg);

/**
 * Create a symbolic link operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fobject File object.
 * \param attr File attributes.
 */
/*
 * [한국어]
 * spdk_fsdev_symlink_cpl_cb - symlink 완료 콜백.
 *
 * @fobject: 새로 생성된 symlink의 inode(lookup count 1로 반환).
 * @attr: 새 symlink의 attr.
 */
typedef void (spdk_fsdev_symlink_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr);

/**
 * Create a symbolic link
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param target symbolic link's content
 * \param linkpath symbolic link's name
 * \param euid Effective user ID of the calling process.
 * \param egid Effective group ID of the calling process.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_symlink - 심볼릭 링크 생성(FUSE SYMLINK).
 *
 * @parent_fobject: 새 링크가 들어갈 디렉토리.
 * @target: 링크가 가리킬 경로(임의 문자열, 존재 여부 검증 안 함).
 * @linkpath: 링크 자체의 이름.
 * @euid/@egid: 게스트 프로세스의 effective UID/GID — 백엔드가 권한 체크와 새 inode의 소유자
 *              결정에 사용. virtio-fs target은 게스트 capability에서 추출한 값을 전달.
 *
 * 호출 체인:
 *   FUSE_SYMLINK → [spdk_fsdev_symlink] → 백엔드 symlinkat
 */
int spdk_fsdev_symlink(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		       struct spdk_fsdev_file_object *parent_fobject, const char *target,
		       const char *linkpath, uid_t euid, gid_t egid,
		       spdk_fsdev_symlink_cpl_cb cb_fn, void *cb_arg);

/**
 * Create file node operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fobject File object.
 * \param attr File attributes.
 */
/*
 * [한국어]
 * spdk_fsdev_mknod_cpl_cb - mknod 완료 콜백.
 *
 * @fobject/@attr: 생성된 노드의 inode(lookup count 1)와 attr.
 */
typedef void (spdk_fsdev_mknod_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				       struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr);

/**
 * Create file node
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param name File name to create.
 * \param mode File type and mode with which to create the new file.
 * \param rdev The device number (only valid if created file is a device)
 * \param euid Effective user ID of the calling process.
 * \param egid Effective group ID of the calling process.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_mknod - 일반 파일/디바이스 노드/FIFO/소켓 생성(FUSE MKNOD, mknod(2)).
 *
 * @mode: 타입(S_IFREG/S_IFCHR/S_IFBLK/S_IFIFO/S_IFSOCK) | 권한 비트.
 * @rdev: 디바이스 노드일 때 (major,minor) 인코딩(makedev(3)). 일반 파일은 0.
 * @euid/@egid: 권한/소유자 결정용.
 *
 * virtio-fs 환경에선 디바이스 노드 생성이 거부될 수 있다(보안 정책).
 */
int spdk_fsdev_mknod(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *parent_fobject, const char *name, mode_t mode, dev_t rdev,
		     uid_t euid, gid_t egid, spdk_fsdev_mknod_cpl_cb cb_fn, void *cb_arg);

/**
 * Create a directory operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fobject File object.
 * \param attr File attributes.
 */
/*
 * [한국어]
 * spdk_fsdev_mkdir_cpl_cb - mkdir 완료 콜백.
 *
 * @fobject/@attr: 새 디렉토리의 inode(lookup count 1) 및 attr.
 */
typedef void (spdk_fsdev_mkdir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				       struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr);

/**
 * Create a directory
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param name Directory name to create.
 * \param mode Directory type and mode with which to create the new directory.
 * \param euid Effective user ID of the calling process.
 * \param egid Effective group ID of the calling process.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_mkdir - 디렉토리 생성(FUSE MKDIR, mkdirat(2)).
 *
 * @mode: 타입은 자동으로 S_IFDIR가 되고 권한 비트는 마스크된다(umask는 별도로 적용되지 않음 —
 *        호출자가 미리 적용해 보내거나 백엔드가 처리).
 */
int spdk_fsdev_mkdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *parent_fobject, const char *name, mode_t mode,
		     uid_t euid, gid_t egid, spdk_fsdev_mkdir_cpl_cb cb_fn, void *cb_arg);


/**
 * Remove a file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_unlink_cpl_cb - unlink 완료 콜백. status만 전달.
 */
typedef void (spdk_fsdev_unlink_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Remove a file
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param name Name to remove.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_unlink - 디렉토리 entry 제거(FUSE UNLINK, unlinkat(2)).
 *
 * 의미: 마지막 hard link면 실제 파일 데이터도 회수 대상이 된다(open된 fhandle은 close 시까지 유지).
 */
int spdk_fsdev_unlink(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *parent_fobject, const char *name,
		      spdk_fsdev_unlink_cpl_cb cb_fn, void *cb_arg);

/**
 * Remove a directory operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_rmdir_cpl_cb - rmdir 완료 콜백.
 */
typedef void (spdk_fsdev_rmdir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Remove a directory
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param name Name to remove.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_rmdir - 빈 디렉토리 제거(FUSE RMDIR, rmdir(2)).
 *
 * 디렉토리가 비어있지 않으면 백엔드가 -ENOTEMPTY 반환.
 */
int spdk_fsdev_rmdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *parent_fobject, const char *name,
		     spdk_fsdev_rmdir_cpl_cb cb_fn, void *cb_arg);

/**
 * Rename a file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_rename_cpl_cb - rename 완료 콜백.
 */
typedef void (spdk_fsdev_rename_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Rename a file
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory.
 * \param name Old rename.
 * \param new_parent_fobject New parent directory.
 * \param new_name New name.
 * \param flags Operation flags.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_rename - 파일/디렉토리 이름 변경 또는 이동(FUSE RENAME/RENAME2, renameat2(2)).
 *
 * @parent_fobject + @name: 원본 위치.
 * @new_parent_fobject + @new_name: 대상 위치.
 * @flags: renameat2(2) 플래그(RENAME_NOREPLACE, RENAME_EXCHANGE, RENAME_WHITEOUT).
 *
 * 의미: parent_fobject == new_parent_fobject 인 경우 단순 rename. 다르면 cross-directory 이동.
 * 동일 fsdev 내에서만 가능(cross-fsdev rename은 미지원).
 */
int spdk_fsdev_rename(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *parent_fobject, const char *name,
		      struct spdk_fsdev_file_object *new_parent_fobject, const char *new_name,
		      uint32_t flags, spdk_fsdev_rename_cpl_cb cb_fn, void *cb_arg);

/**
 * Create a hard link operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fobject File object.
 * \param attr File attributes.
 */
/*
 * [한국어]
 * spdk_fsdev_link_cpl_cb - hard link 생성 완료 콜백.
 *
 * @fobject: 동일한 inode를 가리키는 새 entry의 fobject(보통 원본과 같지만 lookup count 갱신).
 * @attr: 갱신된 attr(nlink가 +1 된 값).
 */
typedef void (spdk_fsdev_link_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				      struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr);

/**
 * Create a hard link
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param new_parent_fobject New parent directory.
 * \param name Link name.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_link - 하드 링크 생성(FUSE LINK, linkat(2)).
 *
 * @fobject: 원본 inode(이미 존재하는 파일).
 * @new_parent_fobject: 새 entry가 들어갈 디렉토리.
 * @name: 새 entry 이름.
 *
 * 디렉토리에 대한 하드 링크는 보안상 거부된다(-EPERM). cross-fsdev hard link도 불가.
 */
int spdk_fsdev_link(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		    struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_object *new_parent_fobject,
		    const char *name, spdk_fsdev_link_cpl_cb cb_fn, void *cb_arg);

/**
 * Get file system statistic operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param statfs filesystem statistics
 */
/*
 * [한국어]
 * spdk_fsdev_statfs_cpl_cb - statfs 완료 콜백. statfs 포인터는 콜백 동안만 유효.
 */
typedef void (spdk_fsdev_statfs_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
					const struct spdk_fsdev_file_statfs *statfs);

/**
 * Get file system statistics
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_statfs - 파일시스템 사용량/한도 조회(FUSE STATFS, statvfs(2)/statfs(2)).
 *
 * @fobject: 마운트 내 임의의 inode. 보통 root_fobject를 사용. */
int spdk_fsdev_statfs(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *fobject, spdk_fsdev_statfs_cpl_cb cb_fn, void *cb_arg);

/**
 * Set an extended attribute operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_setxattr_cpl_cb - setxattr 완료 콜백. status만 전달.
 */
typedef void (spdk_fsdev_setxattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Set an extended attribute
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param name Name of an extended attribute.
 * \param value Buffer that contains value of an extended attribute.
 * \param size Size of an extended attribute.
 * \param flags Operation flags.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_setxattr - 확장 속성(xattr) 설정(FUSE SETXATTR, setxattr(2)).
 *
 * @name: NUL-종결 xattr 이름(예: "user.foo", "security.selinux").
 * @value/@size: xattr 값 버퍼 + 크기. value는 콜백 호출까지 유효해야 함.
 * @flags: XATTR_CREATE(존재 시 실패), XATTR_REPLACE(부재 시 실패), 0(둘 다).
 *
 * 보안: virtio-fs target은 보통 "trusted." 네임스페이스를 게스트에 노출하지 않는다.
 */
int spdk_fsdev_setxattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t unique, struct spdk_fsdev_file_object *fobject, const char *name, const char *value,
			size_t size, uint32_t flags, spdk_fsdev_setxattr_cpl_cb cb_fn, void *cb_arg);
/**
 * Get an extended attribute operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param value_size Size of an data copied to the value buffer.
 */
/*
 * [한국어]
 * spdk_fsdev_getxattr_cpl_cb - getxattr 완료 콜백.
 *
 * @value_size: buffer에 실제로 복사된 바이트 수. status==0 일 때만 유효.
 *              호출 시 size==0이면 백엔드는 실제 xattr 길이만 채우고(-ERANGE 미반환), 호출자가
 *              버퍼를 다시 잡아 재요청. */
typedef void (spdk_fsdev_getxattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		size_t value_size);

/**
 * Get an extended attribute
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param name Name of an extended attribute.
 * \param buffer Buffer to put the extended attribute's value.
 * \param size Size of value's buffer.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_getxattr - 확장 속성 조회(FUSE GETXATTR, getxattr(2)).
 *
 * @buffer/@size: 결과 버퍼와 그 크기. size==0이면 길이만 알아온다.
 * 결과 길이는 cb의 value_size로 통보. 버퍼 부족이면 -ERANGE.
 */
int spdk_fsdev_getxattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t unique, struct spdk_fsdev_file_object *fobject, const char *name, void *buffer,
			size_t size, spdk_fsdev_getxattr_cpl_cb cb_fn, void *cb_arg);

/**
 * List extended attribute names operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param size Size of an extended attribute list.
 * \param size_only true if buffer was NULL or size was 0 upon the \ref spdk_fsdev_listxattr call
 */
/*
 * [한국어]
 * spdk_fsdev_listxattr_cpl_cb - listxattr 완료 콜백.
 *
 * @size: NUL-종결로 구분된 이름 리스트의 총 바이트 수.
 * @size_only: true면 호출 시 버퍼가 비어있어 길이만 측정한 결과(데이터 복사 없음).
 */
typedef void (spdk_fsdev_listxattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		size_t size, bool size_only);

/**
 * List extended attribute names
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param buffer Buffer to to be used for the attribute names.
 * \param size Size of the \b buffer.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_listxattr - 확장 속성 이름 목록 조회(FUSE LISTXATTR, listxattr(2)).
 *
 * @buffer/@size: 결과 버퍼/크기. size==0이면 size_only=true로 길이만 측정.
 * 결과는 NUL-종결 문자열들이 연속해 있는 배열(예: "user.foo\0security.selinux\0").
 */
int spdk_fsdev_listxattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			 uint64_t unique, struct spdk_fsdev_file_object *fobject, char *buffer, size_t size,
			 spdk_fsdev_listxattr_cpl_cb cb_fn, void *cb_arg);

/**
 * Remove an extended attribute operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_removexattr_cpl_cb - removexattr 완료 콜백. status만.
 */
typedef void (spdk_fsdev_removexattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch,
		int status);

/**
 * Remove an extended attribute
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param name Name of an extended attribute.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_removexattr - 확장 속성 삭제(FUSE REMOVEXATTR, removexattr(2)).
 *
 * 존재하지 않는 이름이면 -ENODATA(또는 -ENOATTR) 반환.
 */
int spdk_fsdev_removexattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t unique, struct spdk_fsdev_file_object *fobject, const char *name,
			   spdk_fsdev_removexattr_cpl_cb cb_fn, void *cb_arg);

/**
 * Open a file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fhandle File handle
 */
/*
 * [한국어]
 * spdk_fsdev_fopen_cpl_cb - fopen 완료 콜백.
 *
 * @fhandle: 새로 발급된 file handle. release까지 유효. */
typedef void (spdk_fsdev_fopen_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				       struct spdk_fsdev_file_handle *fhandle);

/**
 * Open a file
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param flags Operation flags.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_fopen - 이미 존재하는 파일을 연다(FUSE OPEN, openat(2) 등).
 *
 * @fobject: 대상 inode(lookup으로 얻음).
 * @flags: open(2) flags(O_RDONLY/O_WRONLY/O_RDWR/O_APPEND/O_NONBLOCK 등).
 *
 * 새 fhandle을 발급한다. 같은 fobject에 여러 fhandle이 동시에 존재 가능.
 */
int spdk_fsdev_fopen(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, uint32_t flags, spdk_fsdev_fopen_cpl_cb cb_fn,
		     void *cb_arg);


/**
 * Create and open a file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 * \param fobject File object.
 * \param attr File attributes.
 * \param fhandle File handle.
 */
/*
 * [한국어]
 * spdk_fsdev_create_cpl_cb - create(O_CREAT) 완료 콜백.
 *
 * @fobject/@attr: 새 파일의 inode(lookup count 1)와 attr.
 * @fhandle: 즉시 발급된 file handle — 별도 fopen 불필요.
 */
typedef void (spdk_fsdev_create_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
					struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
					struct spdk_fsdev_file_handle *fhandle);

/**
 * Create and open a file
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param parent_fobject Parent directory
 * \param name Name to create.
 * \param mode File type and mode with which to create the new file.
 * \param flags Operation flags.
 * \param umask Umask of the calling process.
 * \param euid Effective user ID of the calling process.
 * \param egid Effective group ID of the calling process.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 *  -ENOMEM - operation cannot be initiated as there is not enough memory available
 */
/*
 * [한국어]
 * spdk_fsdev_create - 파일을 생성하고 동시에 연다(FUSE CREATE, openat(O_CREAT)).
 *
 * @mode: 생성될 파일의 mode(타입 + 권한). 보통 S_IFREG | 0644 등.
 * @flags: open flags(O_RDWR | O_CREAT | O_EXCL 등).
 * @umask: 게스트 프로세스의 umask — 백엔드는 mode &= ~umask로 보정.
 *
 * 한 번의 round-trip으로 lookup/create/open이 모두 완료되어 효율이 좋다(FUSE 표준).
 */
int spdk_fsdev_create(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		      struct spdk_fsdev_file_object *parent_fobject, const char *name, mode_t mode, uint32_t flags,
		      mode_t umask, uid_t euid, gid_t egid,
		      spdk_fsdev_create_cpl_cb cb_fn, void *cb_arg);

/**
 * Release an open file operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_release_cpl_cb - release 완료 콜백. status만.
 */
typedef void (spdk_fsdev_release_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Release an open file
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_release - 열린 파일 fhandle을 회수한다(FUSE RELEASE, close(2) on backend fd).
 *
 * 호출 후 fhandle은 사용 불가. unlink 이후 release 시 실제 데이터 회수 트리거.
 */
int spdk_fsdev_release(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		       struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		       spdk_fsdev_release_cpl_cb cb_fn, void *cb_arg);

/**
 * Get file attributes operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status Operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param attr file attributes.
 */
/*
 * [한국어]
 * spdk_fsdev_getattr_cpl_cb - getattr 완료 콜백. attr 포인터는 콜백 동안만 유효. */
typedef void (spdk_fsdev_getattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		const struct spdk_fsdev_file_attr *attr);

/**
 * Get file attributes
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_getattr - 파일 메타데이터 조회(FUSE GETATTR, fstatat(2)/fstat(2)).
 *
 * @fhandle: 열린 파일이라면 해당 fh로 fstat 경로, NULL이면 fobject 기반 fstatat. */
int spdk_fsdev_getattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t unique, struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		       spdk_fsdev_getattr_cpl_cb cb_fn, void *cb_arg);

/**
 * Set file attributes operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status Operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param attr file attributes.
 */
/*
 * [한국어]
 * spdk_fsdev_setattr_cpl_cb - setattr 완료 콜백.
 *
 * @attr: 적용 후 갱신된 attr — 클라이언트가 캐시 갱신에 사용.
 */
typedef void (spdk_fsdev_setattr_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		const struct spdk_fsdev_file_attr *attr);

/**
 * Set file attributes
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle
 * \param attr file attributes to set.
 * \param to_set Bit mask of attributes which should be set.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_setattr - 파일 메타데이터 갱신(FUSE SETATTR; chmod/chown/utime/truncate 통합).
 *
 * @attr: 변경할 값들이 채워진 구조체.
 * @to_set: FSDEV_SET_ATTR_* 비트 OR. 켜진 비트만 attr에서 읽혀 적용.
 *
 * 의미: SIZE 비트가 켜져 있으면 truncate 효과 — 데이터 손실 발생 가능.
 *      ATIME_NOW/MTIME_NOW가 켜져 있으면 attr.atime/mtime은 무시된다.
 */
int spdk_fsdev_setattr(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		       struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		       const struct spdk_fsdev_file_attr *attr, uint32_t to_set,
		       spdk_fsdev_setattr_cpl_cb cb_fn, void *cb_arg);

/**
 * Read data operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 * \param data_size Number of bytes read.
 */
/*
 * [한국어]
 * spdk_fsdev_read_cpl_cb - read 완료 콜백.
 *
 * @data_size: 실제로 iovec에 채워진 바이트 수. 요청 size보다 작을 수 있음(EOF/partial read).
 */
typedef void (spdk_fsdev_read_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				      uint32_t data_size);

/**
 * Read data
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle.
 * \param size Number of bytes to read.
 * \param offs Offset to read from.
 * \param flags Operation flags.
 * \param iov Array of iovec to be used for the data.
 * \param iovcnt Size of the \b iov array.
 * \param opts Optional structure with extended File Operation options. If set, this structure must be
 * valid until the operation is completed. `size` member of this structure is used for ABI compatibility and
 * must be set to sizeof(struct spdk_fsdev_io_opts).
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_read - 파일에서 데이터를 읽어 iovec에 채운다(FUSE READ, preadv(2)).
 *
 * @fhandle: 미리 fopen/create로 발급된 핸들.
 * @size: 요청 바이트 수(상한). mount_opts.max_write에는 영향 없음(read는 별도 한도).
 * @offs: 절대 오프셋 — pread/preadv 의미. fhandle의 위치는 변경되지 않는다.
 * @flags: FUSE READ flags(예: FUSE_READ_LOCKOWNER 등).
 * @iov/@iovcnt: 결과를 채울 scatter 버퍼 배열. 원소들은 호스트/메모리 도메인 상의 버퍼.
 * @opts: 메모리 도메인 등 확장 옵션. NULL이면 호스트 메모리 가정.
 *
 * zero-copy: virtio-fs target은 게스트 페이지의 GPA를 메모리 도메인 ctx로 넘겨, 백엔드가 그
 * 도메인을 통해 직접 게스트 페이지에 데이터를 복사하도록 요청한다 — host bounce buffer 회피.
 */
int spdk_fsdev_read(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		    struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		    size_t size, uint64_t offs, uint32_t flags,
		    struct iovec *iov, uint32_t iovcnt, struct spdk_fsdev_io_opts *opts,
		    spdk_fsdev_read_cpl_cb cb_fn, void *cb_arg);

/**
 * Write data operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 * \param data_size Number of bytes written.
 */
/*
 * [한국어]
 * spdk_fsdev_write_cpl_cb - write 완료 콜백.
 *
 * @data_size: 실제 디스크/백엔드에 기록된 바이트 수. -ENOSPC 등 시 partial write 가능. */
typedef void (spdk_fsdev_write_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
				       uint32_t data_size);

/**
 * Write data
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle.
 * \param size Number of bytes to write.
 * \param offs Offset to write to.
 * \param flags Operation flags.
 * \param iov Array of iovec to where the data is stored.
 * \param iovcnt Size of the \b iov array.
 * \param opts Optional structure with extended File Operation options. If set, this structure must be
 * valid until the operation is completed. `size` member of this structure is used for ABI compatibility and
 * must be set to sizeof(struct spdk_fsdev_io_opts).
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_write - iovec의 데이터를 파일에 기록한다(FUSE WRITE, pwritev(2)).
 *
 * @size: 요청 바이트 수. mount_opts.max_write보다 작거나 같아야 함(virtio-fs target이 분할).
 * @offs: 절대 오프셋. fhandle의 파일 포지션과 무관.
 * @flags: FUSE WRITE flags(예: FUSE_WRITE_CACHE for writeback, FUSE_WRITE_KILL_PRIV).
 * @iov/@iovcnt: 데이터 소스 버퍼.
 * @opts: 메모리 도메인 등.
 *
 * writeback_cache_enabled가 협상되어 있다면 게스트 페이지 캐시가 dirty 페이지를 모아 큰 단위로
 * 보내므로 throughput이 높아진다. 그러나 일관성 가정이 다름에 주의(cache flush 시점 의존).
 */
int spdk_fsdev_write(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle, size_t size,
		     uint64_t offs, uint64_t flags,
		     const struct iovec *iov, uint32_t iovcnt, struct spdk_fsdev_io_opts *opts,
		     spdk_fsdev_write_cpl_cb cb_fn, void *cb_arg);

/**
 * Synchronize file contents operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_fsync_cpl_cb - fsync 완료 콜백. */
typedef void (spdk_fsdev_fsync_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Synchronize file contents
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle.
 * \param datasync Flag indicating if only data should be flushed.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_fsync - 파일 데이터/메타데이터를 안정 저장소까지 sync(FUSE FSYNC).
 *
 * @datasync: true면 fdatasync(2)(데이터만), false면 fsync(2)(데이터 + 메타).
 */
int spdk_fsdev_fsync(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle, bool datasync,
		     spdk_fsdev_fsync_cpl_cb cb_fn, void *cb_arg);

/**
 * Flush operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_flush_cpl_cb - flush 완료 콜백. */
typedef void (spdk_fsdev_flush_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Flush
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_flush - close(2) 직전에 호출되는 flush(FUSE FLUSH).
 *
 * 의미: fsync와 달리 디스크 sync까지 보장하지 않으며, dup된 fd 매번 호출될 수 있다(POSIX
 * close 시 1회). 백엔드는 errno만 보고하고 캐시는 보존하는 게 일반적.
 */
int spdk_fsdev_flush(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		     spdk_fsdev_flush_cpl_cb cb_fn,
		     void *cb_arg);

/**
 * Open a directory operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 *  Following parameters should be ignored if status != 0.
 * \param fhandle File handle
 */
/*
 * [한국어]
 * spdk_fsdev_opendir_cpl_cb - opendir 완료 콜백.
 *
 * @fhandle: 디렉토리 스트림용 fh — readdir/releasedir에 사용. */
typedef void (spdk_fsdev_opendir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status,
		struct spdk_fsdev_file_handle *fhandle);

/**
 * Open a directory
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param flags Operation flags.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_opendir - 디렉토리를 열어 디렉토리 스트림 fh를 발급(FUSE OPENDIR, opendir(3)).
 *
 * @flags: 보통 0. 일부 백엔드는 O_DIRECTORY를 강제. */
int spdk_fsdev_opendir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t unique, struct spdk_fsdev_file_object *fobject, uint32_t flags,
		       spdk_fsdev_opendir_cpl_cb cb_fn, void *cb_arg);

/**
 * Read directory per-entry callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param name Name of the entry
 * \param fobject File object. NULL for "." and "..".
 * \param attr File attributes.
 * \param offset Offset of the next entry
 *
 * \return 0 to continue the enumeration, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_readdir_entry_cb - readdir의 entry 단위 콜백(per-entry).
 *
 * @cb_arg/@ch: 표준.
 * @name: entry 이름(NUL-종결).
 * @fobject: entry의 inode. "." 과 ".." 은 fobject가 NULL로 전달된다 — virtio-fs가 별도 처리.
 * @attr: entry attr (readdirplus 동작 시 채워짐, 기본 readdir에서는 백엔드 의존).
 * @offset: 다음 entry의 offset — 다음 readdir 호출의 offset 인자로 그대로 사용한다.
 * @return: 0이면 계속 열거, 음수 errno면 즉시 중단(완료 콜백에 그 값 전달).
 *
 * 호출 컨텍스트: 백엔드가 결과를 받아 fsdev 코어로 디스패치하는 thread(=발급 thread).
 *
 * 호출 체인:
 *   백엔드 readdir → fsdev 코어 → [entry_cb 반복] → cpl_cb_fn
 */
typedef int (spdk_fsdev_readdir_entry_cb)(void *cb_arg, struct spdk_io_channel *ch,
		const char *name, struct spdk_fsdev_file_object *fobject, const struct spdk_fsdev_file_attr *attr,
		off_t offset);

/**
 * Read directory operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_readdir_cpl_cb - readdir 전체 완료 콜백.
 *
 * status는 백엔드 에러 또는 entry_cb가 반환한 음수 errno. */
typedef void (spdk_fsdev_readdir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Read directory
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle
 * \param offset Offset to continue reading the directory stream
 * \param entry_cb_fn Per-entry callback.
 * \param cpl_cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_readdir - 디렉토리 entry를 streaming으로 열거(FUSE READDIR/READDIRPLUS).
 *
 * @offset: 처음에는 0, 이후에는 직전 entry_cb에서 받은 offset을 그대로 사용.
 * @entry_cb_fn: entry마다 호출. 0 반환으로 계속, 음수로 중단.
 * @cpl_cb_fn: 모든 entry 처리 후(또는 중단 시) 1회 호출.
 *
 * FUSE_CAP_READDIRPLUS가 협상되었으면 entry마다 attr가 함께 채워져 lookup round-trip을 절약. */
int spdk_fsdev_readdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t unique, struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		       uint64_t offset,
		       spdk_fsdev_readdir_entry_cb entry_cb_fn, spdk_fsdev_readdir_cpl_cb cpl_cb_fn, void *cb_arg);

/**
 * Open a directory operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_releasedir_cpl_cb - releasedir 완료 콜백.
 * (헤더 doc의 "Open a directory" 표기는 오타로 보이며 실제는 release 의미) */
typedef void (spdk_fsdev_releasedir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch,
		int status);

/**
 * Open a directory
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_releasedir - 디렉토리 스트림 fh 회수(FUSE RELEASEDIR, closedir(3)).
 * 호출 후 fhandle 무효. opendir과 짝을 이룬다. */
int spdk_fsdev_releasedir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			  uint64_t unique, struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
			  spdk_fsdev_releasedir_cpl_cb cb_fn, void *cb_arg);

/**
 * Synchronize directory contents operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_fsyncdir_cpl_cb - fsyncdir 완료 콜백. */
typedef void (spdk_fsdev_fsyncdir_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Synchronize directory contents
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object.
 * \param fhandle File handle
 * \param datasync Flag indicating if only data should be flushed.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_fsyncdir - 디렉토리의 변경 내역을 안정 저장소로 sync(FUSE FSYNCDIR).
 *
 * @datasync: 일반 fsync와 의미 동일(true=fdatasync, false=fsync). 디렉토리에서는 보통 메타가
 *            전부이므로 동작 차이가 작다. */
int spdk_fsdev_fsyncdir(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t unique, struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
			bool datasync,
			spdk_fsdev_fsyncdir_cpl_cb cb_fn, void *cb_arg);

/**
 * Acquire, modify or release a BSD file lock operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_flock_cpl_cb - flock 완료 콜백.
 *
 * status: 0=성공, -EWOULDBLOCK=non-blocking 시 즉시 잠금 불가, -EBADF 등. */
typedef void (spdk_fsdev_flock_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Acquire, modify or release a BSD file lock
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object..
 * \param fhandle File handle.
 * \param operation Lock operation (see man flock, LOCK_NB will always be added).
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_flock - BSD 스타일 파일 락 획득/변경/해제(FUSE SETLK with FLOCK, flock(2)).
 *
 * @operation: LOCK_SH | LOCK_EX | LOCK_UN. fsdev은 항상 LOCK_NB 가정(blocking은 미지원) —
 *             즉시 시도 후 실패 시 -EWOULDBLOCK. SPDK reactor는 단일 thread polling이므로
 *             blocking 락은 reactor stall을 유발하기에 정책적으로 차단된다. */
int spdk_fsdev_flock(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch, uint64_t unique,
		     struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
		     int operation, spdk_fsdev_flock_cpl_cb cb_fn, void *cb_arg);

/**
 * Allocate requested space operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_fallocate_cpl_cb - fallocate 완료 콜백. */
typedef void (spdk_fsdev_fallocate_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Allocate requested space.
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject File object..
 * \param fhandle File handle.
 * \param mode determines the operation to be performed on the given range, see fallocate(2)
 * \param offset starting point for allocated region.
 * \param length size of allocated region.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_fallocate - 파일에 공간을 미리 할당 또는 punch hole 등 수행(FUSE FALLOCATE,
 * fallocate(2)).
 *
 * @mode: fallocate(2) mode 비트.
 *        - 0: 영역을 0으로 채워 할당(default allocate).
 *        - FALLOC_FL_KEEP_SIZE: size 변경 없이 공간만 예약.
 *        - FALLOC_FL_PUNCH_HOLE | KEEP_SIZE: 구멍 뚫기.
 *        - FALLOC_FL_ZERO_RANGE: 범위 0 채우기.
 * @offset/@length: 대상 범위.
 *
 * 백엔드가 미지원이면 -EOPNOTSUPP. */
int spdk_fsdev_fallocate(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			 uint64_t unique, struct spdk_fsdev_file_object *fobject, struct spdk_fsdev_file_handle *fhandle,
			 int mode, off_t offset, off_t length,
			 spdk_fsdev_fallocate_cpl_cb cb_fn, void *cb_arg);

/**
 * Copy a range of data from one file to another operation completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 * \param data_size Number of bytes written.
 */
/*
 * [한국어]
 * spdk_fsdev_copy_file_range_cpl_cb - copy_file_range 완료 콜백.
 *
 * @data_size: 실제 복사된 바이트 수. 짧을 수 있음(EOF/ENOSPC). */
typedef void (spdk_fsdev_copy_file_range_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch,
		int status, uint32_t data_size);

/**
 * Copy a range of data from one file to another.
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique Unique I/O id.
 * \param fobject_in IN File object.
 * \param fhandle_in IN File handle.
 * \param off_in Starting point from were the data should be read.
 * \param fobject_out OUT File object.
 * \param fhandle_out OUT File handle.
 * \param off_out Starting point from were the data should be written.
 * \param len Maximum size of the data to copy.
 * \param flags Operation flags, see the copy_file_range()
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_copy_file_range - 한 파일의 범위를 다른 파일로 복사(FUSE COPY_FILE_RANGE,
 * copy_file_range(2)).
 *
 * @fobject_in/@fhandle_in/@off_in: 소스 파일/핸들/오프셋.
 * @fobject_out/@fhandle_out/@off_out: 대상 파일/핸들/오프셋.
 * @len: 복사 최대 바이트.
 * @flags: 보통 0. 미래 확장용.
 *
 * 의미: 같은 fsdev 내에서 user-space bounce 없이 빠른 복사가 가능. 동일 백엔드 모듈이 둘 다
 * 다룰 때만 -EOPNOTSUPP가 아니다. virtio-fs는 게스트 reflink/copy_file_range를 가속하기 위해
 * 이 API를 사용한다. */
int spdk_fsdev_copy_file_range(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
			       uint64_t unique,
			       struct spdk_fsdev_file_object *fobject_in, struct spdk_fsdev_file_handle *fhandle_in, off_t off_in,
			       struct spdk_fsdev_file_object *fobject_out, struct spdk_fsdev_file_handle *fhandle_out,
			       off_t off_out, size_t len, uint32_t flags,
			       spdk_fsdev_copy_file_range_cpl_cb cb_fn, void *cb_arg);


/**
 * I/O operation abortion completion callback
 *
 * \param cb_arg Context passed to the corresponding spdk_fsdev_ API
 * \param ch I/O channel.
 * \param status operation result. 0 if the operation succeeded, an error code otherwise.
 */
/*
 * [한국어]
 * spdk_fsdev_abort_cpl_cb - abort 완료 콜백.
 *
 * status: 0(중단 시도가 처리됨; 실제 IO가 이미 완료/미존재 포함). */
typedef void (spdk_fsdev_abort_cpl_cb)(void *cb_arg, struct spdk_io_channel *ch, int status);

/**
 * Abort an I/O
 *
 * \param desc Filesystem device descriptor.
 * \param ch I/O channel.
 * \param unique_to_abort Unique I/O id of the IO to abort.
 * \param cb_fn Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - operation cannot be initiated due to a lack of the internal IO objects
 */
/*
 * [한국어]
 * spdk_fsdev_abort - 진행 중인 I/O를 unique 식별자로 중단 시도(FUSE INTERRUPT).
 *
 * @unique_to_abort: 취소 대상 IO의 unique. 일치하는 IO가 없거나 이미 완료되었어도 status=0이
 *                   반환되며, 발견 시 백엔드에 취소 신호를 보낸다.
 *
 * 의미: 백엔드 의존적이며 best-effort. 일부 작업(write to mmap-mapped page 등)은 취소 불가.
 * 호출 체인:
 *   FUSE_INTERRUPT 수신 → [spdk_fsdev_abort] → 백엔드의 cancel handler */
int spdk_fsdev_abort(struct spdk_fsdev_desc *desc, struct spdk_io_channel *ch,
		     uint64_t unique_to_abort, spdk_fsdev_abort_cpl_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
/* [한국어] extern "C" 블록 종료(헤더 시작부의 #ifdef __cplusplus 와 짝). */
}
#endif

#endif /* SPDK_FSDEV_H */
/* [한국어] 헤더 가드(#ifndef SPDK_FSDEV_H) 종료. */
