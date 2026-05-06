/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
/*
 * [한국어 설명] SPDK fsdev backend 모듈 작성자용 API 헤더 (fsdev_module.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK fsdev (filesystem device) 서브시스템의 "백엔드 작성자"용 API 를 정의한다.
 * 사용자(virtio-fs target, vhost-user-fs, NVMe-oF FUSE-style 호스트 등) 가 호출하는 fsdev.h 와 짝을 이루며,
 * 이 헤더는 module/fsdev/aio (호스트 디렉토리를 fsdev 로 노출), virtiofsd 백엔드 등 "fsdev 인스턴스를 만들어
 * 서비스하는 모듈" 이 자기 모듈을 등록하고, FUSE 시멘틱의 모든 파일/디렉토리 작업 콜백을 fn_table 로 채워 제공할 때
 * 사용한다. 모듈은 SPDK_FSDEV_MODULE_REGISTER 매크로로 자기를 spdk_fsdev_module 리스트에 등록하고, 각 fsdev
 * 인스턴스마다 spdk_fsdev_register() 로 fsdev 코어에 자기 자신을 노출한다. 작업 단위로는 spdk_fsdev_io 라는
 * 한 객체를 받아 비동기로 처리하고, 완료 시 spdk_fsdev_io_complete() 로 응답한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK fsdev 스택은 다음과 같이 계층화되어 있다:
 *   [User]  vhost-user-fs / virtio-fs target / NVMe-oF FUSE host
 *      ↓ spdk_fsdev_open / spdk_fsdev_lookup / read / write 등 (fsdev.h)
 *   [Core] lib/fsdev/fsdev.c — fsdev 인스턴스 레지스트리, 채널/디스크립터 관리, IO 라우팅
 *      ↓ fn_table 콜백 (이 파일에서 정의)
 *   [Backend Module] module/fsdev/aio (POSIX AIO+xattr), virtiofsd 등 — 실제 파일시스템 호출
 *      ↓ POSIX syscalls (open, read, getxattr, ...) 또는 별도 프로토콜
 *   [Host filesystem / virtiofsd / 네트워크 FS]
 * 이 파일은 그 중 "Core ↔ Backend Module" 경계의 인터페이스를 정의한다. 호출 방향은 Core → Backend
 * (submit_request, get_io_channel, destruct, write_config_json) 와 Backend → Core (spdk_fsdev_register,
 * spdk_fsdev_io_complete, spdk_fsdev_module_init_done) 양방향이다. 모든 콜백/완료는 SPDK thread/reactor
 * 컨텍스트에서 polled-mode 로 실행되며, 인터럽트 컨텍스트에서는 절대 실행되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 헤더: spdk/fsdev.h (사용자측 enum/struct 공유 — spdk_fsdev_mount_opts, spdk_fsdev_file_attr,
 * spdk_fsdev_status, spdk_fsdev_readdir_entry_cb 등), spdk/queue.h·spdk/tree.h (TAILQ/RB 매크로),
 * spdk/thread.h (spdk_thread, spdk_io_channel, spdk_spinlock), spdk/util.h (SPDK_CONTAINEROF).
 * 이 헤더에 의존하는 코드: 1) lib/fsdev/fsdev.c 코어 — spdk_fsdev_module 리스트와 spdk_fsdev 인스턴스의
 * fn_table 을 통해 IO 를 라우팅; 2) module/fsdev/* 디렉토리의 모든 백엔드 모듈 (.c 파일) — 자신의
 * spdk_fsdev_module 을 정의하고 SPDK_FSDEV_MODULE_REGISTER 로 constructor 등록; 3) 상위 사용자 (vhost-user-fs
 * 등) 는 직접 이 헤더를 보지 않지만 spdk_fsdev_io 를 간접적으로 흘려보낸다. 데이터 흐름: 사용자측 read/write 요청
 * → 코어가 spdk_fsdev_io 를 채워 fn_table->submit_request 호출 → 백엔드가 비동기 작업 시작 → 완료 시
 * spdk_fsdev_io_complete 로 코어에 반환 → 코어가 사용자 cb_fn 호출. 핵심 공유 자료구조는 spdk_fsdev (인스턴스),
 * spdk_fsdev_module (모듈 메타), spdk_fsdev_io (IO 객체) 세 가지이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_fsdev_module: 백엔드 모듈 메타데이터 (init/fini/get_ctx_size/config_json + name).
 * - struct spdk_fsdev_fn_table: 백엔드 vtable — destruct/submit_request/get_io_channel/write_config_json/
 *   get_memory_domains. submit_request 안에서 spdk_fsdev_io->u_in.<op> 를 읽고, 작업이 끝나면
 *   u_out.<op> 를 채워 spdk_fsdev_io_complete 호출.
 * - struct spdk_fsdev: 백엔드가 만들어 코어에 등록하는 fsdev 인스턴스 (name, ctxt, fn_table, refcnt 등).
 * - enum spdk_fsdev_io_type: FUSE-style 작업 종류 33가지 (MOUNT/LOOKUP/GETATTR/READ/WRITE/...).
 * - struct spdk_fsdev_io: IO 객체 — fsdev/type + u_in 입력 union + u_out 출력 union + internal (cb_fn,
 *   unique, status, channel, desc, list 링크). driver_ctx[0] 가변 영역으로 모듈별 컨텍스트 저장.
 * - spdk_fsdev_register / spdk_fsdev_unregister / spdk_fsdev_unregister_by_name: 인스턴스 라이프사이클.
 * - spdk_fsdev_io_complete: 백엔드가 작업 결과를 코어에 보고 (모든 fn_table 콜백의 응답 경로).
 * - spdk_fsdev_module_init_done / spdk_fsdev_destruct_done: 비동기 초기화/destruct 완료 알림.
 * - SPDK_FSDEV_MODULE_REGISTER: __attribute__((constructor)) 로 모듈 자동 등록 매크로.
 */

#ifndef SPDK_FSDEV_MODULE_H
/* [한국어] 다중 include 보호 가드 — 이 헤더가 타 헤더에 의해 transitive 하게 여러 번 포함되어도
 * 구조체/매크로가 재정의되지 않도록 차단한다. */
#define SPDK_FSDEV_MODULE_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 include 묶음 — stdint, stddef, stdbool, sys/uio (iovec) 등 POSIX/C99 표준 헤더를
 * 모은 SPDK 자체 어그리게이터. 이 파일에서 size_t, uint8_t, mode_t, dev_t, off_t, struct iovec 등이 필요. */
#include "spdk/fsdev.h"
/* [한국어] 사용자측 fsdev API — spdk_fsdev_mount_opts, spdk_fsdev_file_attr, spdk_fsdev_file_statfs,
 * spdk_fsdev_io_opts, enum spdk_fsdev_status, spdk_fsdev_readdir_entry_cb typedef 등을 가져온다.
 * 사용자/백엔드가 같은 데이터 표현을 공유해야 IO 협의가 성립한다. */
#include "spdk/queue.h"
/* [한국어] BSD 스타일 큐 매크로 — TAILQ_HEAD/TAILQ_ENTRY/STAILQ_HEAD/STAILQ_ENTRY 정의. spdk_fsdev_module,
 * spdk_fsdev_io 등이 모듈 리스트, per-channel io_submitted 리스트, per-thread cache 리스트에 연결될 때 사용. */
#include "spdk/tree.h"
/* [한국어] BSD 스타일 RB 트리 매크로 — RB_ENTRY 정의. spdk_fsdev_name 노드가 fsdev 이름 → 인스턴스 맵을
 * O(log N) 으로 검색하기 위한 RB 트리 노드 멤버를 갖기 위해 필요. */
#include "spdk/thread.h"
/* [한국어] SPDK thread/reactor/io_channel 추상화 — struct spdk_thread, struct spdk_io_channel,
 * struct spdk_spinlock 정의. fn_table->get_io_channel 의 반환 타입과 fsdev->internal.spinlock 에 필요. */
#include "spdk/util.h"
/* [한국어] SPDK 유틸리티 — SPDK_CONTAINEROF 매크로 정의. spdk_fsdev_io_from_ctx 가 driver_ctx 포인터로부터
 * 둘러싸는 spdk_fsdev_io 본체 포인터를 역산할 때 사용 (offsetof 트릭). */

#ifdef __cplusplus
/* [한국어] C++ 컴파일러로 이 헤더를 인클루드할 때 C 링키지 강제 — name mangling 방지로 SPDK 라이브러리
 * (.so) 의 C 심볼을 그대로 호출 가능하게 한다. */
extern "C" {
#endif

/**
 * Filesystem device I/O
 *
 * This is an I/O that is passed to an spdk_fsdev.
 */
/*
 * [한국어]
 * struct spdk_fsdev_io 의 forward declaration.
 * 본체는 같은 파일 아래쪽에 정의되지만, struct spdk_fsdev_fn_table 의 함수 포인터 시그니처에서 먼저
 * 등장하므로 이 시점에서 incomplete type 으로 선언해 둔다. 백엔드 모듈은 이 객체의 포인터로 IO 를
 * 받아 u_in/u_out union 에 입출력을 주고받는다.
 */
struct spdk_fsdev_io;

/** Filesystem device module */
/*
 * [한국어]
 * struct spdk_fsdev_module — fsdev 백엔드 모듈의 정적 메타데이터 + 모듈 단위 라이프사이클 콜백 묶음.
 *
 * 한 모듈(예: "aio")에 대해 정확히 한 인스턴스가 SPDK_FSDEV_MODULE_REGISTER 로 정의되며, 부팅 시
 * constructor 가 spdk_fsdev_module_list_add 를 통해 글로벌 모듈 리스트에 등록한다.
 * fsdev 코어는 부팅 시 모든 모듈에 대해 module_init() 을 호출하고, 종료 시 module_fini() 를 호출한다.
 * 한 모듈은 여러 spdk_fsdev 인스턴스를 만들 수 있다 (예: aio 모듈이 여러 호스트 디렉토리를 노출).
 *
 * 실행 컨텍스트: 모든 콜백은 SPDK 의 init thread (보통 reactor 0) 에서 호출된다. 비동기 초기화가
 * 필요하면 module_init 은 0 을 즉시 반환하지 말고 작업을 시작한 뒤 비동기 완료 시 spdk_fsdev_module_init_done
 * 을 호출하면 된다.
 *
 * 동기화: 모듈 리스트 자체는 fsdev 코어가 보호한다. 모듈은 자기 ctx 의 동기화만 책임진다.
 */
struct spdk_fsdev_module {
	/**
	 * Initialization function for the module. Called by the fsdev library
	 * during startup.
	 *
	 * Modules are required to define this function.
	 */
	int (*module_init)(void);
	/* [한국어] 모듈 초기화 콜백 — fsdev 라이브러리 부팅 시 한 번 호출.
	 * 설정자: 모듈 작성자가 SPDK_FSDEV_MODULE_REGISTER 로 등록한 spdk_fsdev_module 정의에서 채움.
	 * 호출자: lib/fsdev 의 subsystem init 단계.
	 * 반환값: 0 = 동기 성공, 음수 errno = 실패 (해당 모듈 비활성). 비동기 작업이 필요하면 0 을 반환하지
	 *   않고 작업을 시작한 뒤 spdk_fsdev_module_init_done() 으로 별도 완료 알림 모델을 사용 가능.
	 * 모든 모듈이 반드시 정의해야 함 (NULL 불가). */

	/**
	 * Finish function for the module. Called by the fsdev library
	 * after all fsdevs for all modules have been unregistered.  This allows
	 * the module to do any final cleanup before the fsdev library finishes operation.
	 *
	 * Modules are not required to define this function.
	 */
	void (*module_fini)(void);
	/* [한국어] 모듈 종료 콜백 — fsdev 코어가 모든 인스턴스를 unregister 한 뒤 호출.
	 * 설정자: 모듈 작성자 (옵셔널, NULL 가능).
	 * 호출자: lib/fsdev subsystem fini.
	 * 사용처: 모듈 전역 자원(메모리 풀, 스레드, 외부 라이브러리 핸들) 해제.
	 * 동시성: 단일 init thread 에서만 호출되므로 별도 락 불필요. */

	/**
	 * Function called to return a text string representing the module-level
	 * JSON RPCs required to regenerate the current configuration.  This will
	 * include module-level configuration options, or methods to construct
	 * fsdevs when one RPC may generate multiple fsdevs.
	 *
	 * Per-fsdev JSON RPCs (where one "construct" RPC always creates one fsdev)
	 * may be implemented here, or by the fsdev's write_config_json function -
	 * but not both.  Fsdev module implementers may choose which mechanism to
	 * use based on the module's design.
	 *
	 * \return 0 on success or Fsdev specific negative error code.
	 */
	int (*config_json)(struct spdk_json_write_ctx *w);
	/* [한국어] 모듈 레벨 JSON-RPC 직렬화 콜백 — `spdk_rpc save_config` 같은 호출에 응답해 현재 모듈/인스턴스
	 * 구성을 재생성할 수 있는 RPC 시퀀스를 JSON 으로 출력한다.
	 * 설정자: 모듈 작성자 (옵셔널).
	 * 호출자: lib/fsdev RPC 핸들러 (rpc/fsdev_rpc.c 등).
	 * 매개변수 w: 이미 열려 있는 JSON write 컨텍스트 (SPDK 의 lib/json 추상화). 모듈은 array element 를
	 *   추가하는 형태로 작성한다.
	 * 반환값: 0 성공, 음수 errno 실패.
	 * 주의: per-fsdev write_config_json 과 둘 중 하나만 구현해야 한다 (중복 시 RPC 가 두 번 기록되어
	 *   재구성 시 충돌 발생). */

	/** Name for the module being defined. */
	const char *name;
	/* [한국어] 모듈을 식별하는 ASCII 문자열 (예: "aio", "virtiofs").
	 * 설정자: 모듈 작성자 (정적 문자열 리터럴 권장).
	 * 읽는 자: spdk_fsdev_module_list_find(name) 가 모듈 검색 시 strcmp 비교; RPC 출력 시 모듈 식별자.
	 * 값 범위: NULL 금지, 시스템 내 모듈 간 유일해야 함. */

	/**
	 * Returns the allocation size required for the backend for uses such as local
	 * command structs, local SGL, iovecs, or other user context.
	 */
	int (*get_ctx_size)(void);
	/* [한국어] per-IO driver_ctx 영역 크기 계산 콜백.
	 * fsdev 코어가 spdk_fsdev_io 를 풀에서 할당할 때, 이 함수가 반환한 바이트 수만큼 추가로 driver_ctx[]
	 * 가변 배열 뒤에 할당한다 (각 IO 마다 모듈 전용 컨텍스트). 모듈은 driver_ctx 포인터에 자신의
	 * per-IO 구조체 (예: AIO 큐 항목, 콜백 인자) 를 캐스팅하여 사용.
	 * 설정자: 모듈 작성자 (NULL 이면 0 으로 간주).
	 * 호출자: spdk_fsdev_io 풀 초기화 시 단 한 번 (정적 크기 계산).
	 * 반환값: 바이트 수 (>=0). 모든 모듈에 대한 최댓값을 코어가 사용. */

	/**
	 * Fields that are used by the internal fsdev subsystem. Fsdev modules
	 *  must not read or write to these fields.
	 */
	struct __fsdev_module_internal_fields {
		TAILQ_ENTRY(spdk_fsdev_module) tailq;
		/* [한국어] 글로벌 모듈 리스트 (g_fsdev_mgr.fsdev_modules) 의 TAILQ 링크.
		 * 설정자: spdk_fsdev_module_list_add (constructor 시점).
		 * 읽는 자: spdk_fsdev_module_list_find 와 init/fini 순회 루프 (lib/fsdev/fsdev.c).
		 * 동기화: 모듈 등록은 모두 main 시작 직전 constructor 에서 단일 스레드로 일어나므로 락 불필요. */
	} internal;
	/* [한국어] fsdev 코어 전용 내부 필드 묶음. 모듈은 이 필드를 직접 접근하지 않는다.
	 * 별도 익명 구조체로 감싼 이유: 외부 모듈이 실수로 tailq 에 직접 LINK 매크로를 호출해 리스트를
	 * 깨뜨리는 것을 명시적으로 방지하기 위함. */
};

/*
 * [한국어]
 * spdk_fsdev_unregister_cb — fsdev 비동기 unregister 완료 콜백 typedef.
 *
 * 설정자/호출 경로: 사용자/모듈이 spdk_fsdev_unregister(fsdev, cb_fn, cb_arg) 또는
 *   spdk_fsdev_unregister_by_name(...) 으로 등록. 모든 디스크립터가 닫히고 fn_table->destruct 가
 *   완료된 뒤 fsdev 코어가 이 콜백을 호출한다.
 * @cb_arg: 사용자가 unregister 호출 시 넘긴 임의 컨텍스트.
 * @rc: 0 = 성공, 음수 errno = destruct 실패.
 * 실행 컨텍스트: fsdev 코어의 내부 메시지 핸들러 — 일반적으로 unregister 를 호출한 스레드와 동일한
 *   스레드에서 실행됨 (spdk_thread_send_msg 경유 가능).
 */
typedef void (*spdk_fsdev_unregister_cb)(void *cb_arg, int rc);

/**
 * Function table for a filesystem device backend.
 *
 * The backend filesystem device function table provides a set of APIs to allow
 * communication with a backend.
 */
/*
 * [한국어]
 * struct spdk_fsdev_fn_table — fsdev 백엔드의 vtable.
 *
 * 한 spdk_fsdev 인스턴스가 가리키는 콜백 묶음. 코어는 이 테이블의 함수 포인터를 통해 백엔드와 통신한다.
 * 핵심 콜백은 submit_request 하나로, 모든 FUSE-style 작업(LOOKUP/GETATTR/READ/WRITE/...)이 spdk_fsdev_io
 * 객체로 와서 이 한 함수를 통해 분기 처리된다 (io->type 으로 분기). 백엔드는 작업을 비동기로 시작하고,
 * 완료 시 spdk_fsdev_io_complete() 를 호출해 코어에 응답한다.
 *
 * 실행 컨텍스트: 모든 콜백은 IO 가 제출된 그 SPDK thread 에서 호출된다 (cross-thread 호출 불가).
 * IO 채널(get_io_channel) 도 호출 스레드별로 별도 인스턴스가 만들어지므로, 백엔드는 채널 레벨에서
 * 락 없이 자료구조를 다룰 수 있다 (lockless polled-mode 의 핵심).
 *
 * 동기화: destruct 는 single-shot 이며 모든 채널이 정리된 뒤 호출. submit_request 는 채널/스레드
 * 단위로 직렬화된다.
 */
struct spdk_fsdev_fn_table {
	/** Destroy the backend filesystem device object */
	int (*destruct)(void *ctx);
	/* [한국어] 백엔드 인스턴스 파괴 콜백.
	 * @ctx: spdk_fsdev->ctxt — 모듈이 register 시 채워둔 자기 컨텍스트 포인터.
	 * 호출자: spdk_fsdev_unregister 경로에서 모든 디스크립터가 닫힌 뒤 fsdev 코어.
	 * 반환값: 0 = 동기 destruct 완료 (즉시 unregister 콜백으로 전이), 1 = 비동기 destruct 시작 —
	 *   백엔드가 나중에 spdk_fsdev_destruct_done() 을 호출해 완료를 알려야 함, 음수 = 에러.
	 * 책임: ctx 내부 자원(파일 디스크립터, 스레드, 메모리) 모두 해제. */

	/** Process the I/O request. */
	void (*submit_request)(struct spdk_io_channel *ch, struct spdk_fsdev_io *);
	/* [한국어] IO 제출 콜백 — 모든 FUSE 작업의 단일 진입점.
	 * @ch: 백엔드가 get_io_channel 로 만들어 준 채널 (스레드 로컬 컨텍스트).
	 * @arg2: spdk_fsdev_io* — io->internal.type 으로 작업 종류 분기, io->u_in.<op> 에서 입력 읽기,
	 *   io->u_out.<op> 에 출력 채우기, 끝나면 spdk_fsdev_io_complete(io, status) 호출.
	 * 실행 컨텍스트: IO 가 제출된 SPDK thread (channel 의 소유 스레드). 인터럽트 컨텍스트 아님.
	 * 비동기 모델: 함수는 즉시 반환해야 하며, 실제 작업 완료는 별도 poller/event 에서.
	 * 동기 완료 가능: in_submit_request 플래그가 true 인 동안 동기적으로 spdk_fsdev_io_complete 를
	 *   호출하면 코어는 deferred completion 으로 처리해 stack overflow/recursion 을 막는다. */

	/** Get an I/O channel for the specific fsdev for the calling thread. */
	struct spdk_io_channel *(*get_io_channel)(void *ctx);
	/* [한국어] 호출 스레드용 IO 채널 발급 콜백.
	 * @ctx: spdk_fsdev->ctxt.
	 * 호출자: 사용자가 spdk_fsdev_get_io_channel(desc) 호출 시, 코어가 이 콜백으로 위임.
	 * 반환값: spdk_io_channel — 보통 spdk_get_io_channel(io_device) 로 만든 스레드 로컬 채널.
	 * 책임: 백엔드는 spdk_io_device_register 로 io_device 를 등록해 두고, 이 콜백에서 spdk_get_io_channel 로
	 *   현재 스레드용 채널 객체를 발급한다. 같은 스레드에서 두 번 호출되면 같은 채널이 반환됨 (refcnt). */

	/**
	 * Output fsdev-specific RPC configuration to a JSON stream. Optional - may be NULL.
	 *
	 * The JSON write context will be initialized with an open object, so the fsdev
	 * driver should write all data necessary to recreate this fsdev by invoking
	 * constructor method. No other data should be written.
	 */
	void (*write_config_json)(struct spdk_fsdev *fsdev, struct spdk_json_write_ctx *w);
	/* [한국어] per-fsdev JSON-RPC 구성 직렬화 콜백 (옵셔널).
	 * 설정자: 모듈 작성자 (NULL 가능).
	 * 호출자: RPC `save_config` 등에서 fsdev 인스턴스마다 호출.
	 * @fsdev: 직렬화 대상 fsdev 인스턴스.
	 * @w: JSON write 컨텍스트 — 호출자가 이미 object 를 열어 두었으므로 모듈은 그 안에 key/value 만 쓴다.
	 * 주의: 모듈 레벨 config_json 과 둘 중 하나만 구현. 둘 다 구현하면 재구성 시 RPC 가 중복 발사된다. */

	/** Get memory domains used by fsdev. Optional - may be NULL.
	 * Vfsdev module implementation should call \ref spdk_fsdev_get_memory_domains for underlying fsdev.
	 * Vfsdev module must inspect types of memory domains returned by base fsdev and report only those
	 * memory domains that it can work with. */
	int (*get_memory_domains)(void *ctx, struct spdk_memory_domain **domains, int array_size);
	/* [한국어] 백엔드가 지원하는 메모리 도메인 조회 콜백 (옵셔널).
	 * @ctx: spdk_fsdev->ctxt.
	 * @domains: 출력 배열 (NULL 가능 — 그 경우 카운트만 조회).
	 * @array_size: 배열 크기.
	 * 반환값: 백엔드가 지원하는 도메인 총 개수 (배열에 다 못 채웠어도 전체 수 반환). 음수면 에러.
	 * 사용처: RDMA/CUDA 메모리 도메인을 알려야 zero-copy IO (spdk_memory_domain_translate_data) 가
	 *   가능. 가상 fsdev (vfsdev) 가 베이스 fsdev 위에 쌓일 때, 자신이 처리 가능한 도메인 교집합만 반환. */
};

/**
 * Filesystem device IO completion callback.
 *
 * \param fsdev_io Filesystem device I/O that has completed.
 * \param cb_arg Callback argument specified when fsdev_io was submitted.
 */
/*
 * [한국어]
 * spdk_fsdev_io_completion_cb — 사용자측 IO 완료 콜백 typedef.
 * @fsdev_io: 완료된 IO. 사용자는 spdk_fsdev_io_get_status (혹은 fsdev.h 의 헬퍼) 로 결과 확인.
 * @cb_arg: 사용자가 IO 제출 시 넘긴 임의 컨텍스트.
 * 실행 컨텍스트: IO 가 제출된 그 SPDK thread (코어가 cross-thread 면 spdk_thread_send_msg 로 라우팅).
 * 호출 경로: 백엔드 spdk_fsdev_io_complete → 코어 → 이 cb_fn → 사용자가 free_io 호출.
 */
typedef void (*spdk_fsdev_io_completion_cb)(struct spdk_fsdev_io *fsdev_io, void *cb_arg);

/*
 * [한국어]
 * struct spdk_fsdev_name — fsdev 인스턴스의 이름 RB-트리 노드.
 *
 * fsdev 코어는 이름 → 인스턴스 매핑을 RB 트리로 유지해 spdk_fsdev_get_by_name 를 O(log N) 으로 처리한다.
 * 각 spdk_fsdev->internal.fsdev_name 에 임베드되어 있다. 백엔드는 직접 다루지 않는다.
 */
struct spdk_fsdev_name {
	char *name;
	/* [한국어] fsdev 의 이름 사본 (heap-allocated, register 시 strdup).
	 * 설정자: spdk_fsdev_register.
	 * 읽는 자: RB 트리 비교 함수 (strcmp), RPC 출력.
	 * 수명: register ~ unregister 완료 시점까지. unregister 후 free. */
	struct spdk_fsdev *fsdev;
	/* [한국어] RB 노드 → fsdev 본체로의 back-pointer (search 결과 즉시 인스턴스 획득용).
	 * 설정자: spdk_fsdev_register.
	 * 동기화: 트리 자체는 fsdev 코어 글로벌 락으로 보호. */
	RB_ENTRY(spdk_fsdev_name) node;
	/* [한국어] BSD RB 트리 링크 멤버 — 좌/우 자식 포인터, 색깔 비트 등 매크로 내부 구현.
	 * 설정자/읽는 자: RB_INSERT/RB_REMOVE/RB_FIND 매크로 (lib/fsdev). 사용자/모듈은 직접 접근 금지. */
};

/*
 * [한국어]
 * fsdev_io_tailq_t / fsdev_io_stailq_t — spdk_fsdev_io 의 TAILQ/STAILQ head 타입 alias.
 *
 * 코어와 모듈이 IO 리스트(예: 채널의 outstanding IO 리스트, per-thread cache STAILQ)를 들고 다닐 때
 * 매번 TAILQ_HEAD(...) 를 풀어 쓰지 않도록 typedef.
 */
typedef TAILQ_HEAD(, spdk_fsdev_io) fsdev_io_tailq_t;
/* [한국어] 양방향 큐 (insert/remove 어디서든 O(1)) — 채널의 in-flight IO 추적에 사용. */
typedef STAILQ_HEAD(, spdk_fsdev_io) fsdev_io_stailq_t;
/* [한국어] 단방향 큐 (head 추가/제거 O(1), tail 추가는 별도 포인터 필요) — per-thread buf cache 처럼
 * 한쪽 방향 큐로 충분한 케이스에 사용. */

/*
 * [한국어]
 * struct spdk_fsdev_file_handle — FUSE 의 fh (file handle) 에 해당하는 불투명 객체.
 * 백엔드가 open/opendir 시 만들어 spdk_fsdev_io_complete_open 등으로 사용자에게 반환하는 핸들로,
 * 후속 read/write/release 콜에서 같은 포인터가 다시 들어온다. 본체 구조는 백엔드별 정의 (불투명).
 *
 * struct spdk_fsdev_file_object — FUSE 의 nodeid 에 해당하는 불투명 객체.
 * lookup/mkdir/symlink/mknod 등으로 만들어지고, FUSE FORGET 의 nlookup 카운트 만큼 참조가 유지된다.
 * 백엔드는 자기 내부에서 이 객체와 inode/path 의 매핑을 관리해야 한다.
 *
 * 두 타입 모두 forward declaration 만 노출되며, 본체는 백엔드 구현체 안에서만 정의된다.
 */
struct spdk_fsdev_file_handle;
struct spdk_fsdev_file_object;


/** The node ID of the root inode */
#define SPDK_FUSE_ROOT_ID 1 /* Must be the same as FUSE_ROOT_ID in the fuse_kernel.h to avoid translation */
/* [한국어] FUSE 루트 inode 의 노드 ID 매크로 (값 1).
 * 왜 1 인가: Linux fuse_kernel.h 의 FUSE_ROOT_ID 와 정확히 일치시켜, virtio-fs/FUSE 프로토콜로
 * 흘러온 nodeid 를 그대로 사용할 수 있게 한다 (재변환 불필요).
 * 사용처: 사용자 측에서 root 디렉토리를 가리키는 file_object 를 만들 때 백엔드가 이 ID 로 mount 의
 *   root_fobject 를 발행. */

/*
 * [한국어]
 * struct spdk_fsdev — fsdev 인스턴스 본체. 백엔드 모듈이 채워서 spdk_fsdev_register 로 코어에 등록한다.
 *
 * 이 구조체는 fsdev.h 에는 incomplete type 으로만 노출되고, 본체 정의가 이 헤더에만 있는 이유는
 * "모듈 작성자만 본체 필드를 채울 수 있어야 하기 때문"이다. 사용자는 spdk_fsdev_get_name(fsdev) 같은
 * 헬퍼만 사용 가능.
 *
 * 라이프사이클: 모듈이 calloc 으로 할당 → ctxt/name/module/fn_table 채움 → spdk_fsdev_register 호출
 *   → 사용자가 open/close → 모듈이 spdk_fsdev_unregister 호출 → 코어가 fn_table->destruct 호출 →
 *   모든 자원 해제 → 모듈이 free.
 * 동기화: internal.spinlock 으로 internal 필드 일부 보호. fn_table 콜백은 IO 채널/스레드 레벨에서 직렬화.
 */
struct spdk_fsdev {
	/** User context passed in by the backend */
	void *ctxt;
	/* [한국어] 백엔드 전용 컨텍스트 포인터 — 모듈이 register 전에 자기 자료구조 (예: aio_fsdev->root_fd,
	 * 마운트 포인트 경로) 를 가리키도록 채운다.
	 * 설정자: 백엔드 모듈 (register 전).
	 * 읽는 자: fn_table 콜백들 — destruct(ctx)/get_io_channel(ctx)/get_memory_domains(ctx) 의 ctx 인자로
	 *   다시 전달됨. submit_request 에서는 io->fsdev->ctxt 로 접근.
	 * 수명: register ~ destruct 완료까지 유효. */

	/** Unique name for this filesystem device. */
	char *name;
	/* [한국어] 인스턴스 식별자 문자열 (예: "AioFs0").
	 * 설정자: 백엔드 모듈 (register 전, strdup 권장).
	 * 읽는 자: spdk_fsdev_get_by_name, RPC 출력, JSON config.
	 * 값 범위: NULL 금지, 시스템 내 fsdev 간 유일. */

	/**
	 * Pointer to the fsdev module that registered this fsdev.
	 */
	struct spdk_fsdev_module *module;
	/* [한국어] 이 인스턴스를 등록한 모듈 포인터 — 코어가 unregister/destruct 시 모듈을 식별하기 위해 사용.
	 * 설정자: 백엔드 모듈 (보통 SPDK_FSDEV_MODULE_REGISTER 로 만든 글로벌 spdk_fsdev_module 의 주소를 대입).
	 * 읽는 자: spdk_fsdev_unregister_by_name 의 모듈 일치 검사, RPC 출력. */

	/** function table for all ops */
	const struct spdk_fsdev_fn_table *fn_table;
	/* [한국어] 백엔드 vtable 포인터 — 모든 FUSE 작업 콜백이 들어 있는 테이블 (위 spdk_fsdev_fn_table).
	 * 설정자: 백엔드 모듈 (register 전).
	 * 읽는 자: 코어의 IO submit/destruct/io_channel 경로.
	 * const 인 이유: 보통 정적 테이블을 가리키고 런타임에 바뀌지 않음 (불변). */

	/** Fields that are used internally by the fsdev subsystem. Fsdev modules
	 *  must not read or write to these fields.
	 */
	struct __fsdev_internal_fields {
		/** Lock protecting fsdev */
		struct spdk_spinlock spinlock;
		/* [한국어] 인스턴스 상태(status, open_descs 리스트, unregister_cb)를 보호하는 spinlock.
		 * SPDK 의 spinlock 은 스레드 안전 (여러 reactor 가 같은 fsdev 를 동시에 open/close 시 보호).
		 * 설정자: 코어 (register 시 init).
		 * 동기화 대상: 아래 status/unregister_cb/unregister_ctx/open_descs 필드. */

		/** The fsdev status */
		enum spdk_fsdev_status status;
		/* [한국어] 인스턴스 상태 머신 — READY/UNREGISTERING/REMOVING 등 (enum 정의는 fsdev.h).
		 * 설정자: 코어 (register=READY, unregister=UNREGISTERING, destruct 완료=REMOVING).
		 * 읽는 자: 새 open 시도 차단 등 — UNREGISTERING/REMOVING 면 -ENODEV 반환.
		 * 동기화: spinlock 보호. */

		/** Callback function that will be called after fsdev destruct is completed. */
		spdk_fsdev_unregister_cb unregister_cb;
		/* [한국어] unregister 완료 시 호출할 사용자 콜백.
		 * 설정자: spdk_fsdev_unregister 가 인자로 받은 cb_fn 을 저장.
		 * 읽는 자: destruct 완료 경로에서 한 번 호출 후 NULL 로 클리어.
		 * 동기화: spinlock 보호. */

		/** Unregister call context */
		void *unregister_ctx;
		/* [한국어] unregister_cb 에 전달할 사용자 컨텍스트 (cb_arg).
		 * 설정자/읽는 자: 위 unregister_cb 와 동일 시점.
		 * 동기화: spinlock 보호. */

		/** List of open descriptors for this filesystem device. */
		TAILQ_HEAD(, spdk_fsdev_desc) open_descs;
		/* [한국어] 이 인스턴스에 대해 현재 열려 있는 디스크립터 리스트.
		 * 설정자: spdk_fsdev_open 시 push, spdk_fsdev_close 시 remove.
		 * 읽는 자: unregister 시 모든 디스크립터를 순회하며 hot-remove 알림 → 사용자가 close 호출하도록 유도.
		 * 동기화: spinlock 보호 (cross-thread 호출 가능). */

		TAILQ_ENTRY(spdk_fsdev) link;
		/* [한국어] 코어의 글로벌 fsdev 리스트 (g_fsdev_mgr.fsdevs) 의 TAILQ 링크.
		 * 설정자: spdk_fsdev_register 시 INSERT, unregister 완료 시 REMOVE.
		 * 읽는 자: spdk_fsdev_first/spdk_fsdev_next 순회, RPC `bdev_get_iostat` 비슷한 list 호출.
		 * 동기화: 코어의 글로벌 락 (g_fsdev_mgr.mutex) 보호. */

		/** Fsdev name used for quick lookup */
		struct spdk_fsdev_name fsdev_name;
		/* [한국어] 이름 → fsdev RB 트리 노드 (위에서 정의한 spdk_fsdev_name).
		 * 설정자: spdk_fsdev_register 시 RB_INSERT.
		 * 읽는 자: spdk_fsdev_get_by_name 의 RB_FIND.
		 * 동기화: 코어 글로벌 락 보호. */
	} internal;
	/* [한국어] fsdev 코어 전용 내부 필드 묶음 — 모듈은 이 필드를 직접 읽거나 쓰지 않는다.
	 * 별도 구조체로 감싼 이유는 (1) 외부에서 실수로 접근하지 않도록 명시, (2) 향후 ABI 호환을 위해
	 * 내부 필드만 별도 진화 가능. */
};

/*
 * [한국어]
 * enum spdk_fsdev_io_type — fsdev IO 의 작업 종류 식별자 (FUSE 오퍼레이션 매핑).
 *
 * 이 enum 의 값들은 FUSE 프로토콜 (fuse_kernel.h 의 fuse_opcode) 과 1:1 대응되며, virtio-fs/FUSE 호스트가
 * 받은 요청을 spdk_fsdev_io->internal.type 에 채워 백엔드에 전달한다. 백엔드의 submit_request 는 이 값으로
 * switch 하여 u_in.<해당 작업> 필드를 읽고, 작업 종료 시 u_out.<해당 작업> 을 채운다.
 *
 * 값의 순서는 ABI 의 일부가 아니지만, __SPDK_FSDEV_IO_LAST 는 항상 마지막에 두어 enum 개수 셀 때 사용한다.
 */
enum spdk_fsdev_io_type {
	SPDK_FSDEV_IO_MOUNT,
	/* [한국어] 마운트 협상 — 사용자(예: virtiofsd 호스트) 와 백엔드가 capability/option 을 교환.
	 * u_in.mount.opts 입력, u_out.mount.opts/root_fobject 출력. 백엔드는 자신이 지원하지 않는 capability
	 * 비트를 클리어해 사용자에게 돌려줌으로써 비활성화 가능. */
	SPDK_FSDEV_IO_UMOUNT,
	/* [한국어] 언마운트 — 모든 핸들/객체가 닫힌 뒤 호출. 백엔드는 마운트 상태 정리. */
	SPDK_FSDEV_IO_LOOKUP,
	/* [한국어] FUSE LOOKUP — parent_fobject + name 으로 자식 inode 를 찾고 file_object + attr 반환.
	 * 성공 시 lookup 카운트 1 증가 (백엔드가 추적). 실패 시 -ENOENT. */
	SPDK_FSDEV_IO_FORGET,
	/* [한국어] FUSE FORGET — 사용자가 nlookup 만큼 file_object 의 참조를 감소시키라고 통지.
	 * 백엔드는 누적 카운트가 0 이 되면 객체 free 가능. 응답 없는(원래 fire-and-forget) 메시지지만
	 * SPDK 에서는 다른 IO 와 동일하게 spdk_fsdev_io_complete 로 응답 처리. */
	SPDK_FSDEV_IO_GETATTR,
	/* [한국어] FUSE GETATTR — fobject (또는 fhandle) 의 stat 정보 조회. u_out.getattr.attr 채움. */
	SPDK_FSDEV_IO_SETATTR,
	/* [한국어] FUSE SETATTR — 속성 변경 (chmod/chown/utimens/truncate). to_set 비트마스크가 변경 항목 표시. */
	SPDK_FSDEV_IO_READLINK,
	/* [한국어] FUSE READLINK — 심볼릭 링크 타겟 문자열 반환. linkname 은 fsdev 레이어가 free. */
	SPDK_FSDEV_IO_SYMLINK,
	/* [한국어] FUSE SYMLINK — parent 아래 linkpath 라는 이름으로 target 을 가리키는 심링크 생성. */
	SPDK_FSDEV_IO_MKNOD,
	/* [한국어] FUSE MKNOD — 디바이스 노드 또는 일반 파일 생성. mode/rdev 로 종류 결정. */
	SPDK_FSDEV_IO_MKDIR,
	/* [한국어] FUSE MKDIR — 디렉토리 생성. */
	SPDK_FSDEV_IO_UNLINK,
	/* [한국어] FUSE UNLINK — 일반 파일 삭제. */
	SPDK_FSDEV_IO_RMDIR,
	/* [한국어] FUSE RMDIR — 빈 디렉토리 삭제. */
	SPDK_FSDEV_IO_RENAME,
	/* [한국어] FUSE RENAME — (parent, name) → (new_parent, new_name) 으로 이름 변경/이동. flags 는
	 * RENAME_NOREPLACE/EXCHANGE/WHITEOUT 비트 (renameat2(2) 와 동일). */
	SPDK_FSDEV_IO_LINK,
	/* [한국어] FUSE LINK — 하드링크 생성. fobject 를 new_parent 아래 name 으로 새 링크. */
	SPDK_FSDEV_IO_OPEN,
	/* [한국어] FUSE OPEN — 일반 파일 open. flags 는 O_RDONLY/O_RDWR 등. fhandle 반환. */
	SPDK_FSDEV_IO_READ,
	/* [한국어] FUSE READ — fhandle/offs/size 로 데이터 읽기. iov 에 결과 기록, data_size 출력. */
	SPDK_FSDEV_IO_WRITE,
	/* [한국어] FUSE WRITE — 데이터 쓰기. iov 입력, data_size 출력 (실제로 쓴 바이트). */
	SPDK_FSDEV_IO_STATFS,
	/* [한국어] FUSE STATFS — 파일시스템 통계 (block size, free blocks 등) 조회. */
	SPDK_FSDEV_IO_RELEASE,
	/* [한국어] FUSE RELEASE — open 된 파일 핸들 닫기 (close(2) 와 유사). */
	SPDK_FSDEV_IO_FSYNC,
	/* [한국어] FUSE FSYNC — 파일 데이터/메타데이터를 디스크에 동기화. datasync=true 면 데이터만. */
	SPDK_FSDEV_IO_SETXATTR,
	/* [한국어] FUSE SETXATTR — 확장 속성 (xattr) 설정. flags 는 XATTR_CREATE/XATTR_REPLACE. */
	SPDK_FSDEV_IO_GETXATTR,
	/* [한국어] FUSE GETXATTR — xattr 조회. value_size 출력 (size=0 호출로 크기만 조회 가능). */
	SPDK_FSDEV_IO_LISTXATTR,
	/* [한국어] FUSE LISTXATTR — xattr 이름 목록 조회. size_only=true 면 크기만 보고. */
	SPDK_FSDEV_IO_REMOVEXATTR,
	/* [한국어] FUSE REMOVEXATTR — xattr 삭제. */
	SPDK_FSDEV_IO_FLUSH,
	/* [한국어] FUSE FLUSH — close(2) 직전에 호출 (실제 close 는 RELEASE). 보통 cache flush. */
	SPDK_FSDEV_IO_OPENDIR,
	/* [한국어] FUSE OPENDIR — 디렉토리 open. fhandle 반환. */
	SPDK_FSDEV_IO_READDIR,
	/* [한국어] FUSE READDIR — 디렉토리 엔트리 나열. entry_cb_fn 콜백을 통해 항목별 스트리밍 반환. */
	SPDK_FSDEV_IO_RELEASEDIR,
	/* [한국어] FUSE RELEASEDIR — 디렉토리 핸들 닫기. */
	SPDK_FSDEV_IO_FSYNCDIR,
	/* [한국어] FUSE FSYNCDIR — 디렉토리 메타데이터 동기화. */
	SPDK_FSDEV_IO_FLOCK,
	/* [한국어] FUSE FLOCK — flock(2) 어드바이저리 락. operation 은 LOCK_SH/LOCK_EX/LOCK_UN 등. */
	SPDK_FSDEV_IO_CREATE,
	/* [한국어] FUSE CREATE — open(O_CREAT|O_EXCL) 형태의 원자적 생성+open. attr+fhandle 동시 반환. */
	SPDK_FSDEV_IO_ABORT,
	/* [한국어] FUSE INTERRUPT 비슷한 작업 — unique_to_abort 로 식별되는 진행 중 IO 를 취소.
	 * 백엔드는 best-effort 로 처리, 이미 완료된 IO 면 무시. */
	SPDK_FSDEV_IO_FALLOCATE,
	/* [한국어] FUSE FALLOCATE — 영역 미리 할당. mode 는 FALLOC_FL_KEEP_SIZE/PUNCH_HOLE 등. */
	SPDK_FSDEV_IO_COPY_FILE_RANGE,
	/* [한국어] copy_file_range(2) — 두 파일 사이에 커널 공간에서 직접 복사 (zero-copy 가능). */
	__SPDK_FSDEV_IO_LAST
	/* [한국어] enum 카운트 sentinel — 배열 크기/검증용. 새 IO 타입은 반드시 이 앞에 추가.
	 * 더블 언더스코어 prefix 는 "내부용, ABI 일부 아님" 을 의미. */
};

/*
 * [한국어]
 * struct spdk_fsdev_io — fsdev 한 IO 요청 객체. 코어와 백엔드가 공유하는 핵심 자료구조.
 *
 * 라이프사이클: 사용자가 fsdev.h 의 헬퍼 (예: spdk_fsdev_read) 호출 → 코어가 mempool 에서 spdk_fsdev_io 를
 *   할당하고 fsdev/type/u_in 을 채움 → fn_table->submit_request 호출 → 백엔드가 비동기 작업 진행 중에
 *   io->driver_ctx (모듈 전용 영역) 사용 → 작업 완료 시 백엔드가 u_out 채우고 spdk_fsdev_io_complete 호출 →
 *   코어가 internal.cb_fn(io, cb_arg) 으로 사용자 콜백 호출 → 사용자가 spdk_fsdev_free_io 로 풀에 반환.
 *
 * 메모리 레이아웃: union u_in/u_out 은 동시에 한 종류만 유효 (type 으로 분기). driver_ctx[0] 은 가변 길이
 *   배열 트릭으로 구조체 끝에 모듈 전용 영역을 붙이며, get_ctx_size() 가 그 크기를 반환한다.
 *
 * 동기화: 한 IO 는 채널/스레드에 묶여 있어 단일 스레드 컨텍스트에서만 다뤄진다 (lockless). cross-thread
 *   완료가 필요하면 코어가 spdk_thread_send_msg 로 라우팅.
 */
struct spdk_fsdev_io {
	/** The filesystem device that this I/O belongs to. */
	struct spdk_fsdev *fsdev;
	/* [한국어] 이 IO 가 속한 fsdev 인스턴스 포인터.
	 * 설정자: 코어 (IO 할당 시).
	 * 읽는 자: 백엔드 submit_request 가 io->fsdev->ctxt 로 자기 컨텍스트 회수, 또는 type-specific 분기.
	 * 동기화: IO 가 살아 있는 동안 fsdev 가 unregister 되지 않음을 코어가 보장 (descriptor refcnt). */

	/** Enumerated value representing the I/O type. */
	uint8_t type;
	/* [한국어] enum spdk_fsdev_io_type 값 — 어떤 작업인지 식별 (uint8_t 로 패킹).
	 * 설정자: 코어 (IO 발행 시).
	 * 읽는 자: 백엔드 submit_request 의 switch 분기, 디버그 트레이스.
	 * 주의: internal.type 도 같은 값을 보유 (편의/접근성용 중복). */

	/** A single iovec element for use by this fsdev_io. */
	struct iovec iov;
	/* [한국어] 단일 iovec 슬롯 — 백엔드가 일시적으로 한 버퍼를 표현해야 할 때 사용 (예: get_buf 로 받은 큰 버퍼).
	 * 설정자: 백엔드 (필요 시).
	 * 읽는 자: 백엔드 자신.
	 * 주의: read/write 의 진짜 데이터 iov 는 u_in.read.iov / u_in.write.iov 에 별도로 있음 (그쪽이 본체). */

	/*
	 * [한국어]
	 * union u_in — 입력 인자 union. type 에 따라 정확히 한 멤버만 유효.
	 * 코어가 IO 발행 시 채우고, 백엔드 submit_request 가 읽는다. 모든 포인터형 (name, target, buffer 등) 의
	 * 수명은 IO 가 완료될 때까지 (코어가 보장).
	 */
	union {
		struct {
			struct spdk_fsdev_mount_opts opts;
			/* [한국어] 마운트 시 협상하려는 옵션/capability 비트. 백엔드가 일부를 클리어해 응답으로 돌릴 수 있음. */
		} mount;
		/* [한국어] MOUNT 입력 — 사용자가 요청한 마운트 옵션. */
		struct {
			struct spdk_fsdev_file_object *parent_fobject;
			/* [한국어] 부모 디렉토리 객체 (lookup 시작 지점). 루트면 백엔드가 mount 시 발급한 root_fobject. */
			char *name;
			/* [한국어] 찾을 자식 이름 (NULL-terminated). 코어가 소유 (IO 동안 유효). */
		} lookup;
		/* [한국어] LOOKUP 입력 — 부모 + 이름으로 자식 inode 검색. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] forget 대상 객체. */
			uint64_t nlookup;
			/* [한국어] 감소시킬 lookup 참조 카운트. 백엔드는 누적값을 정확히 일치시켜야 (안 그러면 inode 누수
			 * 또는 use-after-free). FUSE 프로토콜이 요구하는 핵심 의미론. */
		} forget;
		/* [한국어] FORGET 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] stat 대상 객체. */
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] 열린 핸들이 있으면 fstat-style 호출 (없으면 NULL → stat-style). */
		} getattr;
		/* [한국어] GETATTR 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] 변경 대상 객체. */
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] 열린 핸들 (옵셔널, ftruncate 시 사용). */
			struct spdk_fsdev_file_attr attr;
			/* [한국어] 새 속성 값 (mode/uid/gid/size/atime/mtime/ctime). to_set 으로 표시된 필드만 적용. */
			uint32_t to_set;
			/* [한국어] 변경할 필드 비트마스크 (FATTR_MODE/UID/GID/SIZE/ATIME/MTIME/CTIME 등). */
		} setattr;
		/* [한국어] SETATTR 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] 심링크 객체. */
		} readlink;
		/* [한국어] READLINK 입력. */
		struct {
			struct spdk_fsdev_file_object *parent_fobject;
			/* [한국어] 새 심링크가 만들어질 부모 디렉토리. */
			char *target;
			/* [한국어] 심링크가 가리킬 경로 문자열. */
			char *linkpath;
			/* [한국어] 새로 만들 심링크의 이름. */
			uid_t euid;
			/* [한국어] 생성 주체의 effective UID — virtio-fs 등에서 게스트 사용자 권한 매핑용. */
			gid_t egid;
			/* [한국어] 생성 주체의 effective GID. */
		} symlink;
		/* [한국어] SYMLINK 입력. */
		struct {
			struct spdk_fsdev_file_object *parent_fobject;
			/* [한국어] 부모 디렉토리. */
			char *name;
			/* [한국어] 새 노드 이름. */
			mode_t mode;
			/* [한국어] 파일 종류 + 권한 (S_IFREG|0644, S_IFCHR|0666 등). */
			dev_t rdev;
			/* [한국어] 디바이스 노드일 때 major/minor 인코딩 (mknod(2) 와 동일). */
			uid_t euid;
			/* [한국어] 생성자 effective UID. */
			gid_t egid;
			/* [한국어] 생성자 effective GID. */
		} mknod;
		/* [한국어] MKNOD 입력. */
		struct {
			struct spdk_fsdev_file_object *parent_fobject;
			/* [한국어] 부모 디렉토리. */
			char *name;
			/* [한국어] 새 디렉토리 이름. */
			mode_t mode;
			/* [한국어] 권한 (umask 적용 후의 최종 mode). */
			uid_t euid;
			gid_t egid;
			/* [한국어] 생성자 effective UID/GID. */
		} mkdir;
		/* [한국어] MKDIR 입력. */
		struct {
			struct spdk_fsdev_file_object *parent_fobject;
			/* [한국어] 삭제 대상 부모 디렉토리. */
			char *name;
			/* [한국어] 삭제할 항목 이름. */
		} unlink;
		/* [한국어] UNLINK 입력. */
		struct {
			struct spdk_fsdev_file_object *parent_fobject;
			/* [한국어] 부모 디렉토리. */
			char *name;
			/* [한국어] 삭제할 디렉토리 이름. */
		} rmdir;
		/* [한국어] RMDIR 입력. */
		struct {
			struct spdk_fsdev_file_object *parent_fobject;
			/* [한국어] 원본 위치 부모. */
			char *name;
			/* [한국어] 원본 이름. */
			struct spdk_fsdev_file_object *new_parent_fobject;
			/* [한국어] 새 위치 부모 (같은 디렉토리면 parent_fobject 와 동일). */
			char *new_name;
			/* [한국어] 새 이름. */
			uint32_t flags;
			/* [한국어] renameat2(2) flags — RENAME_NOREPLACE/EXCHANGE/WHITEOUT. */
		} rename;
		/* [한국어] RENAME 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] 링크 대상이 될 기존 파일 객체. */
			struct spdk_fsdev_file_object *new_parent_fobject;
			/* [한국어] 새 링크가 만들어질 부모 디렉토리. */
			char *name;
			/* [한국어] 새 링크 이름. */
		} link;
		/* [한국어] LINK 입력 (하드링크). */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] open 할 파일 객체. */
			uint32_t flags;
			/* [한국어] open(2) flags — O_RDONLY/O_RDWR/O_APPEND/O_DIRECT 등. */
		} open;
		/* [한국어] OPEN 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] 읽을 파일 객체. */
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] open 으로 받은 핸들. */
			size_t size;
			/* [한국어] 읽고 싶은 바이트 수 (요청). 실제 읽은 바이트는 u_out.read.data_size 로 응답. */
			uint64_t offs;
			/* [한국어] 시작 오프셋 (파일 내 바이트 위치). */
			uint32_t flags;
			/* [한국어] FUSE_READ flags (예: FUSE_READ_LOCKOWNER). */
			struct iovec *iov;
			/* [한국어] 사용자가 결과를 받을 iov 배열 (사용자 소유 메모리, 백엔드는 채워 넣음). */
			uint32_t iovcnt;
			/* [한국어] iov 배열 길이. */
			struct spdk_fsdev_io_opts *opts;
			/* [한국어] 추가 옵션 (memory_domain 등 zero-copy 정보, 옵셔널). */
		} read;
		/* [한국어] READ 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] 쓸 파일 객체. */
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] open 으로 받은 핸들. */
			size_t size;
			/* [한국어] 쓸 바이트 수 (요청). */
			uint64_t offs;
			/* [한국어] 시작 오프셋. */
			uint64_t flags;
			/* [한국어] FUSE_WRITE flags (FUSE_WRITE_CACHE/LOCKOWNER 등). */
			const struct iovec *iov;
			/* [한국어] 쓸 데이터를 담은 iov (read 와 달리 const — 백엔드가 수정 금지). */
			uint32_t iovcnt;
			/* [한국어] iov 길이. */
			struct spdk_fsdev_io_opts *opts;
			/* [한국어] 추가 옵션. */
		} write;
		/* [한국어] WRITE 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] statfs 대상 (보통 root_fobject). */
		} statfs;
		/* [한국어] STATFS 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] 닫을 파일 객체. */
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] 닫을 핸들. */
		} release;
		/* [한국어] RELEASE 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] fsync 대상. */
			bool datasync;
			/* [한국어] true 면 fdatasync(2) 시멘틱 (메타데이터 제외). */
		} fsync;
		/* [한국어] FSYNC 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] xattr 설정 대상. */
			char *name;
			/* [한국어] xattr 이름 (예: "user.tag"). */
			char *value;
			/* [한국어] xattr 값 바이너리. */
			size_t size;
			/* [한국어] value 바이트 길이. */
			uint32_t flags;
			/* [한국어] XATTR_CREATE/XATTR_REPLACE 비트. */
		} setxattr;
		/* [한국어] SETXATTR 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] 대상 객체. */
			char *name;
			/* [한국어] 조회할 xattr 이름. */
			void *buffer;
			/* [한국어] 결과를 담을 사용자 버퍼. NULL 이면 size 만 조회. */
			size_t size;
			/* [한국어] buffer 의 용량. */
		} getxattr;
		/* [한국어] GETXATTR 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] 대상 객체. */
			char *buffer;
			/* [한국어] xattr 이름 목록 (NUL-separated) 을 담을 버퍼. */
			size_t size;
			/* [한국어] buffer 용량. 0 이면 size_only 호출. */
		} listxattr;
		/* [한국어] LISTXATTR 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] 대상 객체. */
			char *name;
			/* [한국어] 삭제할 xattr 이름. */
		} removexattr;
		/* [한국어] REMOVEXATTR 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] flush 대상 객체/핸들. */
		} flush;
		/* [한국어] FLUSH 입력 (close 직전). */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] open 할 디렉토리 객체. */
			uint32_t flags;
			/* [한국어] open flags (O_DIRECTORY 자동, 추가 비트는 백엔드별). */
		} opendir;
		/* [한국어] OPENDIR 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] 디렉토리 객체/핸들. */
			uint64_t offset;
			/* [한국어] 시작 위치 (이전 readdir 응답에서 받은 다음-오프셋). */
			int (*entry_cb_fn)(struct spdk_fsdev_io *fsdev_io, void *cb_arg);
			/* [한국어] 코어가 백엔드에 전달하는 항목별 콜백 — 백엔드가 한 엔트리를 u_out.readdir 에
			 * 채우고 이 함수를 호출하면 코어가 사용자 콜백으로 전달. 0 반환 = 다음 항목 계속, 음수 = 중단. */
			spdk_fsdev_readdir_entry_cb *usr_entry_cb_fn;
			/* [한국어] 사용자(상위 호출자)의 항목별 콜백 (포인터의 포인터 형태로 보관). 코어가 entry_cb_fn
			 * 안에서 이를 호출. */
		} readdir;
		/* [한국어] READDIR 입력 (다른 IO 와 달리 항목별 스트리밍). */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] releasedir 대상. */
		} releasedir;
		/* [한국어] RELEASEDIR 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] fsyncdir 대상. */
			bool datasync;
			/* [한국어] true 면 데이터만 sync. */
		} fsyncdir;
		/* [한국어] FSYNCDIR 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] flock 대상 핸들. */
			int operation; /* see man flock */
			/* [한국어] LOCK_SH/LOCK_EX/LOCK_UN [|LOCK_NB] — flock(2) 인자와 동일. */
		} flock;
		/* [한국어] FLOCK 입력. */
		struct {
			struct spdk_fsdev_file_object *parent_fobject;
			/* [한국어] 부모 디렉토리. */
			char *name;
			/* [한국어] 새 파일 이름. */
			mode_t mode;
			/* [한국어] 생성 권한. */
			uint32_t flags;
			/* [한국어] open flags (O_CREAT 묵시 — O_EXCL/O_TRUNC 등 추가). */
			mode_t umask;
			/* [한국어] 호출자 umask — 권한 마스킹용. */
			uid_t euid;
			gid_t egid;
			/* [한국어] 생성자 effective UID/GID. */
		} create;
		/* [한국어] CREATE 입력 (atomic create+open). */
		struct {
			uint64_t unique_to_abort;
			/* [한국어] 취소할 IO 의 unique 값 (FUSE INTERRUPT 의 reqid). 백엔드는 자기 outstanding 리스트에서
			 * 매칭되는 IO 를 찾아 best-effort 로 취소. */
		} abort;
		/* [한국어] ABORT 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] fallocate 대상. */
			int mode;
			/* [한국어] FALLOC_FL_KEEP_SIZE/PUNCH_HOLE/COLLAPSE_RANGE 등 비트. */
			off_t offset;
			off_t length;
			/* [한국어] 영역 시작/길이. */
		} fallocate;
		/* [한국어] FALLOCATE 입력. */
		struct {
			struct spdk_fsdev_file_object *fobject_in;
			struct spdk_fsdev_file_handle *fhandle_in;
			off_t off_in;
			/* [한국어] 원본 파일/핸들/오프셋. */
			struct spdk_fsdev_file_object *fobject_out;
			struct spdk_fsdev_file_handle *fhandle_out;
			off_t off_out;
			/* [한국어] 대상 파일/핸들/오프셋. */
			size_t len;
			/* [한국어] 복사할 바이트 수. */
			uint32_t flags;
			/* [한국어] copy_file_range(2) flags (현재는 0 만 정의). */
		} copy_file_range;
		/* [한국어] COPY_FILE_RANGE 입력 (in-kernel zero-copy 가능 경로). */
	} u_in;
	/* [한국어] 입력 union 끝 — type 에 해당하는 멤버 하나만 백엔드가 읽는다. */

	/*
	 * [한국어]
	 * union u_out — 출력 union. 백엔드가 작업 결과를 채워 spdk_fsdev_io_complete 전에 기록한다.
	 * 모든 작업 종류가 응답 데이터를 갖는 것은 아니므로 (예: UNLINK/FORGET/UMOUNT) u_out 에는 일부 작업만 존재.
	 */
	union {
		struct {
			struct spdk_fsdev_mount_opts opts;
			/* [한국어] 백엔드가 협상 후 확정한 capability/option (사용자가 제안한 것에서 일부 비활성 가능). */
			struct spdk_fsdev_file_object *root_fobject;
			/* [한국어] 마운트 후 사용자가 lookup 등의 시작점으로 쓸 root file_object. SPDK_FUSE_ROOT_ID 와 매핑. */
		} mount;
		/* [한국어] MOUNT 출력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] lookup 결과 자식 객체. ENOENT 시 NULL. */
			struct spdk_fsdev_file_attr attr;
			/* [한국어] 자식의 stat 정보. */
		} lookup;
		/* [한국어] LOOKUP 출력. */
		struct {
			struct spdk_fsdev_file_attr attr;
			/* [한국어] 조회된 stat. */
		} getattr;
		/* [한국어] GETATTR 출력. */
		struct {
			struct spdk_fsdev_file_attr attr;
			/* [한국어] 변경 후 stat. */
		} setattr;
		/* [한국어] SETATTR 출력. */
		struct {
			char *linkname; /* will be freed by the fsdev layer */
			/* [한국어] 심링크 타겟 문자열. 메모리 소유권: 백엔드가 strdup/malloc 해서 채우면 fsdev 코어가
			 * 사용자 콜백 후 free 한다 (모듈은 이중 free 하지 말 것). */
		} readlink;
		/* [한국어] READLINK 출력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_attr attr;
			/* [한국어] 새로 생성된 심링크 객체와 stat. */
		} symlink;
		/* [한국어] SYMLINK 출력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_attr attr;
			/* [한국어] 새 mknod 객체와 stat. */
		} mknod;
		/* [한국어] MKNOD 출력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_attr attr;
			/* [한국어] 새 디렉토리 객체와 stat. */
		} mkdir;
		/* [한국어] MKDIR 출력. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_attr attr;
			/* [한국어] 하드링크 후 객체 (대상 파일과 동일 inode 의 새 참조) + stat. */
		} link;
		/* [한국어] LINK 출력. */
		struct {
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] 새 파일 핸들. 후속 read/write/release 시 다시 들어옴. */
		} open;
		/* [한국어] OPEN 출력. */
		struct {
			uint32_t data_size;
			/* [한국어] 실제 읽힌 바이트 수 (size 보다 작을 수 있음 — short read/EOF). */
		} read;
		/* [한국어] READ 출력. */
		struct {
			uint32_t data_size;
			/* [한국어] 실제 쓰여진 바이트 수. */
		} write;
		/* [한국어] WRITE 출력. */
		struct {
			struct spdk_fsdev_file_statfs statfs;
			/* [한국어] block size, blocks, bfree, bavail, files, ffree, namelen 등. */
		} statfs;
		/* [한국어] STATFS 출력. */
		struct {
			size_t value_size;
			/* [한국어] 실제 xattr 값 길이. buffer=NULL 호출 시 크기만 확인. */
		} getxattr;
		/* [한국어] GETXATTR 출력. */
		struct {
			size_t data_size;
			/* [한국어] xattr 이름 목록의 총 바이트 수. */
			bool size_only;
			/* [한국어] true 면 백엔드가 buffer 를 안 채우고 size 만 보고 (사용자가 size=0 으로 호출). */
		} listxattr;
		/* [한국어] LISTXATTR 출력. */
		struct {
			struct spdk_fsdev_file_handle *fhandle;
			/* [한국어] 새 디렉토리 핸들. */
		} opendir;
		/* [한국어] OPENDIR 출력. */
		struct {
			const char *name;
			/* [한국어] 현재 엔트리 이름 (백엔드가 가리키는 메모리, 콜백 동안 유효). */
			struct spdk_fsdev_file_object *fobject;
			/* [한국어] 엔트리의 file_object (있으면 lookup 효과 — refcnt 증가). */
			struct spdk_fsdev_file_attr attr;
			/* [한국어] 엔트리 stat. */
			off_t offset;
			/* [한국어] 다음 readdir 호출에 넘길 오프셋 (FUSE 의 d_off). */
		} readdir;
		/* [한국어] READDIR 항목별 출력 — 백엔드가 항목 하나마다 채우고 entry_cb_fn 호출. */
		struct {
			struct spdk_fsdev_file_object *fobject;
			struct spdk_fsdev_file_handle *fhandle;
			struct spdk_fsdev_file_attr attr;
			/* [한국어] CREATE 결과: 새 객체 + 핸들 + stat 한 번에. */
		} create;
		/* [한국어] CREATE 출력. */
		struct {
			size_t data_size;
			/* [한국어] copy_file_range 가 실제로 복사한 바이트 수. */
		} copy_file_range;
		/* [한국어] COPY_FILE_RANGE 출력. */
	} u_out;
	/* [한국어] 출력 union 끝. */

	/**
	 *  Fields that are used internally by the fsdev subsystem. Fsdev modules
	 *  must not read or write to these fields.
	 */
	struct __fsdev_io_internal_fields {
		/** The fsdev I/O channel that this was handled on. */
		struct spdk_fsdev_channel *ch;
		/* [한국어] 이 IO 가 처리되는 코어측 채널 (per-thread).
		 * 설정자: 코어 (IO 발행 시).
		 * 읽는 자: 코어 (완료 처리 시 stat 갱신, in-flight 리스트 제거).
		 * 동기화: 채널은 소유 스레드 1개에 묶여 있음 → lockless. */

		/** The fsdev descriptor that was used when submitting this I/O. */
		struct spdk_fsdev_desc *desc;
		/* [한국어] 이 IO 가 발행된 디스크립터 (사용자가 spdk_fsdev_open 으로 받은 것).
		 * 설정자: 코어 (IO 발행 시).
		 * 읽는 자: 코어가 cb_fn 으로 디스크립터 컨텍스트 복원 시. */

		/** User function that will be called when this completes */
		spdk_fsdev_io_completion_cb cb_fn;
		/* [한국어] 완료 시 호출할 사용자 콜백 (위 typedef).
		 * 설정자: 코어 (사용자가 spdk_fsdev_<op> 호출 시 인자로 받은 것을 저장).
		 * 읽는 자: 코어 — spdk_fsdev_io_complete 가 trampoline 으로 이 함수 호출. */

		/** Context that will be passed to the completion callback */
		void *cb_arg;
		/* [한국어] cb_fn 의 두 번째 인자로 들어갈 사용자 컨텍스트 (요청 식별자, 상위 IO 객체 등). */

		/**
		 * Set to true while the fsdev module submit_request function is in progress.
		 *
		 * This is used to decide whether spdk_fsdev_io_complete() can complete the I/O directly
		 * or if completion must be deferred via an event.
		 */
		bool in_submit_request;
		/* [한국어] submit_request 중 동기적으로 spdk_fsdev_io_complete 가 호출되었는지 추적하는 가드.
		 * 설정자: 코어 (submit_request 진입 시 true, 반환 후 false).
		 * 읽는 자: spdk_fsdev_io_complete — true 면 콜백 호출을 spdk_thread_send_msg 로 deferred 처리해
		 *   submit→complete→cb_fn→submit 의 무한 재귀와 stack overflow 를 회피.
		 * 왜 필요한가: 빠르게 완료되는 in-memory 백엔드(예: malloc)는 동기 완료가 가능한데, 그대로 cb_fn
		 *   을 부르면 cb_fn 이 또 다음 IO 를 submit 하는 패턴에서 스택이 무한 깊어진다. */

		/** IO operation */
		enum spdk_fsdev_io_type type;
		/* [한국어] 작업 타입 (위 type 필드와 같은 값, internal 별도 사본).
		 * 설정자: 코어. */

		/** IO unique ID */
		uint64_t unique;
		/* [한국어] IO 식별자 — FUSE 의 unique 와 매핑. ABORT 가 이 값으로 IO 취소.
		 * 설정자: 코어 (IO 발행 시 단조 증가).
		 * 읽는 자: 디버그 트레이스, abort 매칭. */

		/** User callback */
		void *usr_cb_fn;
		/* [한국어] 상위 사용자(예: vhost-user-fs target) 의 도메인별 콜백을 임시 보관하는 슬롯.
		 * 설정자: 코어 또는 사용자 헬퍼. 일반적으로 fsdev.h 의 작업별 헬퍼가 사용. */

		/** The context for the user callback */
		void *usr_cb_arg;
		/* [한국어] usr_cb_fn 인자. */

		/** Status for the IO */
		int status;
		/* [한국어] IO 결과 (0 성공, 음수 errno).
		 * 설정자: 백엔드가 spdk_fsdev_io_complete(io, status) 호출 시 코어가 저장.
		 * 읽는 자: 사용자 콜백 / 헬퍼 (spdk_fsdev_io_get_status). */

		/** Member used for linking child I/Os together. */
		TAILQ_ENTRY(spdk_fsdev_io) link;
		/* [한국어] 부모-자식 IO 분할 시 형제 리스트 링크 (예: 큰 read 가 여러 작은 read 로 쪼개진 경우).
		 * 설정자/읽는 자: 코어. */

		/** Entry to the list per_thread_cache of struct spdk_fsdev_mgmt_channel. */
		STAILQ_ENTRY(spdk_fsdev_io) buf_link;
		/* [한국어] per-thread buf cache (큰 버퍼 대기 큐) 의 STAILQ 링크.
		 * 설정자/읽는 자: spdk_fsdev_io_get_buf 로 큰 버퍼 요청 시 풀이 부족하면 이 링크로 큐잉. */

		/** Entry to the list io_submitted of struct spdk_fsdev_channel */
		TAILQ_ENTRY(spdk_fsdev_io) ch_link;
		/* [한국어] 채널의 in-flight (제출된) IO 추적 리스트 링크.
		 * 설정자: 코어 (submit 시 push).
		 * 읽는 자: 코어 (complete 시 pop, abort 검색, reset 시 일괄 정리). */
	} internal;
	/* [한국어] 코어 전용 내부 필드 묶음 — 모듈 접근 금지. */

	/**
	 * Per I/O context for use by the fsdev module.
	 */
	uint8_t driver_ctx[0];
	/* [한국어] 가변 길이 배열 트릭 (zero-length array) — 구조체 끝에 모듈 전용 컨텍스트 영역을 붙이기 위한
	 * placeholder. 실제 크기는 spdk_fsdev_module->get_ctx_size() 가 반환한 바이트 수.
	 * 사용법: 백엔드는 (struct my_ctx *)io->driver_ctx 로 캐스팅해 자기 전용 데이터 (예: AIO 큐 항목,
	 *   pthread cond, 부분 결과) 를 저장.
	 * 역방향: spdk_fsdev_io_from_ctx(driver_ctx_ptr) 로 둘러싼 spdk_fsdev_io* 회수 (SPDK_CONTAINEROF). */

	/* No members may be added after driver_ctx! */
	/* [한국어] driver_ctx 가 가변 길이이므로 그 뒤에는 어떤 멤버도 추가할 수 없다 (메모리 오프셋이 동적이라
	 * 컴파일러가 다음 멤버 위치를 결정 못함). */
};

/**
 * Register a new fsdev.
 *
 * \param fsdev Filesystem device to register.
 *
 * \return 0 on success.
 * \return -EINVAL if the fsdev name is NULL.
 * \return -EEXIST if a fsdev with the same name already exists.
 */
/*
 * [한국어]
 * spdk_fsdev_register - 새 fsdev 인스턴스를 코어에 등록.
 *
 * @fsdev: 모듈이 calloc 해서 ctxt/name/module/fn_table 까지 모두 채운 인스턴스.
 * @return: 0 성공, -EINVAL (name NULL), -EEXIST (이름 중복), 기타 음수 errno.
 *
 * 호출 시점: 모듈이 자기 백엔드 인스턴스 하나를 만들어 사용자에게 노출하고 싶을 때 (예: aio 모듈이
 *   RPC `aio_create` 로 호스트 디렉토리를 fsdev 로 노출). module_init 안에서 또는 RPC 핸들러에서 호출.
 * 동작: 글로벌 fsdev 리스트와 RB 트리에 추가, internal 필드 (spinlock, status=READY) 초기화, 채널/통계
 *   메타데이터 등록. 등록 직후부터 사용자가 spdk_fsdev_open 으로 이 fsdev 를 열 수 있다.
 * 실행 컨텍스트: SPDK init thread 또는 RPC thread (코어가 글로벌 락 보호).
 *
 * 호출 체인: <백엔드 모듈 RPC handler> → [spdk_fsdev_register] → 코어 글로벌 리스트 INSERT.
 */
int spdk_fsdev_register(struct spdk_fsdev *fsdev);

/**
 * Start unregistering a fsdev. This will notify each currently open descriptor
 * on this fsdev of the hotremoval to request the upper layers to stop using this fsdev
 * and manually close all the descriptors with spdk_fsdev_close().
 * The actual fsdev unregistration may be deferred until all descriptors are closed.
 *
 * Note: spdk_fsdev_unregister() can be unsafe unless the fsdev is not opened before and
 * closed after unregistration. It is recommended to use spdk_fsdev_unregister_by_name().
 *
 * \param fsdev Filesystem device to unregister.
 * \param cb_fn Callback function to be called when the unregister is complete.
 * \param cb_arg Argument to be supplied to cb_fn
 */
/*
 * [한국어]
 * spdk_fsdev_unregister - fsdev hot-remove 시작 (포인터 직접 지정 버전).
 *
 * @fsdev: unregister 할 인스턴스.
 * @cb_fn: 모든 디스크립터가 닫히고 destruct 가 끝나면 호출되는 완료 콜백.
 * @cb_arg: cb_fn 에 전달할 컨텍스트.
 *
 * 동작 단계:
 *   1) 인스턴스 status 를 UNREGISTERING 으로 변경 (spinlock 보호).
 *   2) open_descs 의 각 디스크립터에 hot-remove 이벤트 발사 → 사용자가 spdk_fsdev_close 호출 유도.
 *   3) 모든 디스크립터가 닫히면 fn_table->destruct 호출.
 *   4) destruct 가 동기 완료(0 반환) 또는 spdk_fsdev_destruct_done 비동기 완료 이후 cb_fn(cb_arg, 0) 호출.
 *
 * 주의: fsdev 포인터를 직접 받으므로 호출 직전 fsdev 가 이미 unregister 되면 use-after-free.
 *   안전한 대안은 spdk_fsdev_unregister_by_name (이름으로 락 잡고 검색).
 *
 * 호출 체인: <RPC `delete` handler> → [spdk_fsdev_unregister] → ... → fn_table->destruct → cb_fn.
 */
void spdk_fsdev_unregister(struct spdk_fsdev *fsdev, spdk_fsdev_unregister_cb cb_fn, void *cb_arg);

/**
 * Start unregistering a fsdev. This will notify each currently open descriptor
 * on this fsdev of the hotremoval to request the upper layer to stop using this fsdev
 * and manually close all the descriptors with spdk_fsdev_close().
 * The actual fsdev unregistration may be deferred until all descriptors are closed.
 *
 * \param fsdev_name Filesystem device name to unregister.
 * \param module Module by which the filesystem device was registered.
 * \param cb_fn Callback function to be called when the unregister is complete.
 * \param cb_arg Argument to be supplied to cb_fn
 *
 * \return 0 on success, or suitable errno value otherwise
 */
/*
 * [한국어]
 * spdk_fsdev_unregister_by_name - 이름 + 모듈로 안전하게 unregister.
 *
 * @fsdev_name: unregister 할 인스턴스 이름.
 * @module: 등록한 모듈 (이중 안전장치 — 다른 모듈이 같은 이름의 인스턴스를 unregister 하지 못하게).
 * @cb_fn / @cb_arg: 완료 콜백.
 * @return: 0 성공, -ENODEV (이름 없음), -EINVAL (모듈 불일치), 기타 음수 errno.
 *
 * 동작: 코어 글로벌 락을 잡고 RB 트리에서 이름 검색 → 찾은 fsdev 의 module 이 인자와 일치하면 unregister
 *   진행. 검색~unregister 시작이 락 안에서 원자적으로 일어나므로 race-free.
 *
 * 호출 체인: <RPC `delete` handler> → [spdk_fsdev_unregister_by_name] → spdk_fsdev_unregister 내부 로직.
 */
int spdk_fsdev_unregister_by_name(const char *fsdev_name, struct spdk_fsdev_module *module,
				  spdk_fsdev_unregister_cb cb_fn, void *cb_arg);

/**
 * Invokes the unregister callback of a fsdev backing a virtual fsdev.
 *
 * A Fsdev with an asynchronous destruct path should return 1 from its
 * destruct function and call this function at the conclusion of that path.
 * Fsdevs with synchronous destruct paths should return 0 from their destruct
 * path.
 *
 * \param fsdev Filesystem device that was destroyed.
 * \param fsdeverrno Error code returned from fsdev's destruct callback.
 */
/*
 * [한국어]
 * spdk_fsdev_destruct_done - 비동기 destruct 의 완료 보고.
 *
 * @fsdev: destruct 가 끝난 인스턴스.
 * @fsdeverrno: 0 성공, 음수 errno 실패.
 *
 * 사용 시점: 백엔드의 fn_table->destruct 가 비동기 작업을 시작하고 1 을 반환한 경우, 그 작업이 끝나는
 *   시점에 백엔드가 이 함수를 호출. 코어는 이 호출을 받아 unregister_cb 사용자 콜백을 발사한다.
 *
 * 동기 destruct 모델: destruct 가 0 을 반환하면 코어가 즉시 다음 단계로 넘어가므로 이 함수 불필요.
 * 비동기 destruct 모델: destruct 가 1 반환 후 백엔드는 자기 자원 정리 (예: 비동기 close, drain) 를
 *   진행하다가 완료 시점에 spdk_fsdev_destruct_done 호출.
 *
 * 호출 체인: <백엔드 비동기 destruct 콜백> → [spdk_fsdev_destruct_done] → 코어 → unregister_cb.
 */
void spdk_fsdev_destruct_done(struct spdk_fsdev *fsdev, int fsdeverrno);

/**
 * Indicate to the fsdev layer that the module is done initializing.
 *
 * To be called once during module_init or asynchronously after
 * an asynchronous operation required for module initialization is completed.
 *
 * \param module Pointer to the module completing the initialization.
 */
/*
 * [한국어]
 * spdk_fsdev_module_init_done - 모듈 초기화 완료를 코어에 알림.
 *
 * @module: 초기화가 끝난 모듈.
 *
 * 사용 시점: module_init 가 비동기 작업 (예: 외부 데몬과 핸드셰이크) 을 시작했을 때, 그 작업의 완료 시점에
 *   호출. 모든 모듈의 init_done 이 모일 때까지 fsdev 서브시스템 init 가 완료되지 않는다 (배리어).
 *
 * 동기 init 모델: module_init 가 0 을 즉시 반환하면 코어가 자동으로 init_done 처리하므로 호출 불필요.
 * 비동기 init 모델: module_init 가 0 반환 후 비동기 작업 시작 → 작업 완료 시 백엔드가 이 함수 호출.
 *
 * 호출 체인: <백엔드 비동기 init 콜백> → [spdk_fsdev_module_init_done] → 코어 → 다음 모듈 init or
 *   subsystem init 완료 알림.
 */
void spdk_fsdev_module_init_done(struct spdk_fsdev_module *module);

/**
 * Complete a fsdev_io
 *
 * \param fsdev_io I/O to complete.
 * \param status The I/O completion status.
 */
/*
 * [한국어]
 * spdk_fsdev_io_complete - 백엔드가 IO 처리 완료를 코어에 보고.
 *
 * @fsdev_io: 완료된 IO 객체. 백엔드는 호출 전에 u_out 을 모두 채워두어야 함.
 * @status: 0 성공, 음수 errno 실패 (사용자 cb_fn 이 받을 결과).
 *
 * 동작:
 *   1) internal.status 에 status 저장.
 *   2) 채널의 io_submitted 리스트에서 제거, 통계 갱신.
 *   3) internal.in_submit_request 가 true 면 즉시 cb_fn 호출 시 stack 폭주 가능성 → spdk_thread_send_msg
 *      로 deferred 호출 (다음 reactor tick).
 *   4) false 면 직접 internal.cb_fn(fsdev_io, internal.cb_arg) 호출.
 *   5) cross-thread (IO submit 스레드 ≠ 현재 스레드) 면 send_msg 로 라우팅.
 *
 * 실행 컨텍스트: 백엔드의 polled-mode 완료 처리 코드 (poller, 콜백) 안.
 * 호출 빈도: 모든 IO 마다 정확히 한 번.
 *
 * 호출 체인: <백엔드 완료 폴러> → [spdk_fsdev_io_complete] → 코어 → 사용자 cb_fn → spdk_fsdev_free_io.
 */
void spdk_fsdev_io_complete(struct spdk_fsdev_io *fsdev_io, int status);


/**
 * Get I/O type
 *
 * \param fsdev_io I/O to complete.
 *
 * \return operation code associated with the I/O
 */
/*
 * [한국어]
 * spdk_fsdev_io_get_type - IO 의 작업 타입 조회 (인라인 헬퍼).
 *
 * @fsdev_io: 조회 대상.
 * @return: enum spdk_fsdev_io_type 값.
 *
 * 사용처: 백엔드의 submit_request 가 switch 분기 전 type 추출. internal.type 을 그대로 반환하므로
 *   io->internal.type 직접 접근과 동치이지만, 캡슐화를 위해 헬퍼 사용 권장.
 * 인라인인 이유: 한 줄짜리 접근자 — 함수 호출 오버헤드를 없애 폴링 hot-path 에 적합.
 */
static inline enum spdk_fsdev_io_type
spdk_fsdev_io_get_type(struct spdk_fsdev_io *fsdev_io) {
	return fsdev_io->internal.type;
	/* [한국어] internal.type 단순 반환 — 외부 모듈이 internal 필드를 직접 보지 않게 하는 캡슐화. */
}

/**
 * Get I/O unique id
 *
 * \param fsdev_io I/O to complete.
 *
 * \return I/O unique id
 */
/*
 * [한국어]
 * spdk_fsdev_io_get_unique - IO 고유 ID 조회 (인라인 헬퍼).
 *
 * @fsdev_io: 조회 대상.
 * @return: uint64 단조 증가 ID (FUSE unique).
 *
 * 사용처: ABORT 와 매칭, 디버그 로그 식별자, 모니터링 트레이스. 백엔드가 자기 outstanding 리스트를 IO
 *   객체 포인터 대신 unique 로 키잉할 때도 활용.
 */
static inline uint64_t
spdk_fsdev_io_get_unique(struct spdk_fsdev_io *fsdev_io)
{
	return fsdev_io->internal.unique;
	/* [한국어] internal.unique 단순 반환. */
}

/**
 * Free an I/O request. This should only be called after the completion callback
 * for the I/O has been called and notifies the fsdev layer that memory may now
 * be released.
 *
 * \param fsdev_io I/O request.
 */
/*
 * [한국어]
 * spdk_fsdev_free_io - IO 객체 해제 (mempool 반환).
 *
 * @fsdev_io: 해제할 IO. 사용자 cb_fn 이 호출된 뒤에만 호출해야 함 (그 전에 free 하면 use-after-free).
 *
 * 동작: 코어가 이 IO 를 할당 시 사용한 mempool 에 반환. driver_ctx 영역도 함께 회수.
 * 실행 컨텍스트: 사용자 cb_fn 안 또는 그 직후 (보통 cb_fn 의 마지막 줄에서 호출).
 * 비동기 IO 모델의 마지막 단계.
 *
 * 호출 체인: <사용자 cb_fn> → [spdk_fsdev_free_io] → mempool put.
 */
void spdk_fsdev_free_io(struct spdk_fsdev_io *fsdev_io);


/**
 * Get a thread that given fsdev_io was submitted on.
 *
 * \param fsdev_io I/O
 * \return thread that submitted the I/O
 */
/*
 * [한국어]
 * spdk_fsdev_io_get_thread - IO 가 제출된 SPDK thread 조회.
 *
 * @fsdev_io: 조회 대상.
 * @return: spdk_thread* — IO 가 발행된 그 스레드. cross-thread 완료 라우팅 시 코어가 이 값을 참조.
 *
 * 사용처: 백엔드가 다른 스레드에서 작업을 진행하다가 완료를 원래 스레드로 돌려보낼 때 (보통 코어가 알아서
 *   처리하지만, 모듈이 자기 메시지로 라우팅이 필요하면 이 헬퍼 사용).
 */
struct spdk_thread *spdk_fsdev_io_get_thread(struct spdk_fsdev_io *fsdev_io);

/**
 * Get the fsdev module's I/O channel that the given fsdev_io was submitted on.
 *
 * \param fsdev_io I/O
 * \return the fsdev module's I/O channel that the given fsdev_io was submitted on.
 */
/*
 * [한국어]
 * spdk_fsdev_io_get_io_channel - IO 의 백엔드 채널 조회.
 *
 * @fsdev_io: 조회 대상.
 * @return: spdk_io_channel* — 백엔드 fn_table->get_io_channel 이 발급했던 그 채널.
 *
 * 사용처: 백엔드 submit_request 가 ch 인자를 못 갖고 있는 경로 (예: 콜백 체인 끝에서 IO 만 들고 있을 때)
 *   에서 채널 컨텍스트를 회수해 자기 자료구조 (예: AIO 컨텍스트) 에 접근.
 */
struct spdk_io_channel *spdk_fsdev_io_get_io_channel(struct spdk_fsdev_io *fsdev_io);

/**
 * Add the given module to the list of registered modules.
 * This function should be invoked by referencing the macro
 * SPDK_FSDEV_MODULE_REGISTER in the module c file.
 *
 * \param fsdev_module Module to be added.
 */
/*
 * [한국어]
 * spdk_fsdev_module_list_add - 모듈을 글로벌 모듈 리스트에 추가.
 *
 * @fsdev_module: 등록할 모듈 메타데이터.
 *
 * 호출 시점: 모듈 .c 파일의 SPDK_FSDEV_MODULE_REGISTER 매크로가 만든 constructor 함수에서 자동 호출.
 *   직접 호출하지 말 것 — 항상 매크로 경유.
 * 동작: g_fsdev_mgr.fsdev_modules TAILQ 에 INSERT_TAIL. 부팅 후 lib/fsdev subsystem init 가 이 리스트를
 *   순회하며 module_init 호출.
 * 실행 컨텍스트: main 진입 전 (constructor 단계) — 단일 스레드, 락 불필요.
 *
 * 호출 체인: <__attribute__((constructor)) 함수> → [spdk_fsdev_module_list_add].
 */
void spdk_fsdev_module_list_add(struct spdk_fsdev_module *fsdev_module);

/**
 * Find registered module with name pointed by \c name.
 *
 * \param name name of module to be searched for.
 * \return pointer to module or NULL if no module with \c name exist
 */
/*
 * [한국어]
 * spdk_fsdev_module_list_find - 이름으로 등록된 모듈 검색.
 *
 * @name: 찾을 모듈 이름 (NULL-terminated).
 * @return: spdk_fsdev_module* 또는 NULL.
 *
 * 사용처: RPC 핸들러가 모듈 이름 파라미터로 모듈 dispatch 할 때, 또는 다른 모듈이 의존 모듈 존재 확인 시.
 * 동작: 글로벌 모듈 리스트를 strcmp 로 선형 검색 (모듈 수가 적으므로 O(N) 충분).
 * 실행 컨텍스트: init 후 어느 스레드든 호출 가능 (리스트는 init 단계에 고정되므로 read-only, 락 불필요).
 */
struct spdk_fsdev_module *spdk_fsdev_module_list_find(const char *name);

/*
 * [한국어]
 * spdk_fsdev_io_from_ctx - driver_ctx 포인터에서 둘러싼 spdk_fsdev_io 회수 (인라인).
 *
 * @ctx: 백엔드가 io->driver_ctx 로 받았던 자기 컨텍스트 포인터.
 * @return: spdk_fsdev_io* — ctx 를 가진 IO 본체.
 *
 * 사용 패턴: 백엔드가 비동기 작업의 콜백 인자로 driver_ctx 포인터만 갖고 있을 때, 이 헬퍼로 IO 본체로
 *   역산하여 spdk_fsdev_io_complete 등을 호출.
 * 구현: SPDK_CONTAINEROF 매크로 — offsetof(struct spdk_fsdev_io, driver_ctx) 만큼 ctx 에서 빼서 본체
 *   포인터 계산. 컴파일 타임 상수 오프셋이라 매우 빠름.
 *
 * 호출 체인: <백엔드 비동기 콜백> → [spdk_fsdev_io_from_ctx] → spdk_fsdev_io_complete.
 */
static inline struct spdk_fsdev_io *
spdk_fsdev_io_from_ctx(void *ctx)
{
	return SPDK_CONTAINEROF(ctx, struct spdk_fsdev_io, driver_ctx);
	/* [한국어] container_of 패턴 — Linux 커널의 동명 매크로와 같은 트릭. ctx 가 spdk_fsdev_io 의
	 * driver_ctx 멤버 주소라는 가정 하에, 그 멤버의 오프셋만큼 뒤로 가서 구조체 시작 주소를 얻는다. */
}

/*
 *  Macro used to register module for later initialization.
 */
/*
 * [한국어]
 * SPDK_FSDEV_MODULE_REGISTER - fsdev 백엔드 모듈을 자동 등록하는 매크로.
 *
 * 사용 예 (예: module/fsdev/aio/fsdev_aio.c 끝):
 *   static struct spdk_fsdev_module aio_fsdev_module = { .name = "aio", .module_init = ... };
 *   SPDK_FSDEV_MODULE_REGISTER(aio, &aio_fsdev_module)
 *
 * 동작: __attribute__((constructor)) 로 main 진입 직전에 자동 실행되는 함수를 만들어, 그 안에서
 *   spdk_fsdev_module_list_add 를 호출. 모듈 작성자가 명시적 등록 코드를 쓸 필요 없음.
 *
 * 매크로 인자:
 *   @name: 함수 이름 충돌 방지를 위한 토큰 (모듈마다 유일). 보통 모듈 이름과 같게 둠.
 *   @module: spdk_fsdev_module 포인터 (정적 변수의 주소).
 *
 * 왜 constructor 인가: SPDK 의 모든 서브시스템 등록은 constructor 패턴으로 통일되어 있어, 사용자가
 *   명시적으로 init 순서를 신경쓰지 않아도 되고, 링커가 .ctors 섹션을 통해 자동 발사한다.
 *
 * static 인 이유: 매크로가 만든 함수가 외부 심볼로 노출되면 같은 모듈을 여러 .c 가 공유 시 중복 정의
 *   에러 — static 이라 TU 로컬.
 */
#define SPDK_FSDEV_MODULE_REGISTER(name, module) \
static void __attribute__((constructor)) _spdk_fsdev_module_register_##name(void) \
{ \
	spdk_fsdev_module_list_add(module); \
}
/* [한국어] 매크로 본체 — 토큰 페이스팅 (##) 으로 함수 이름을 모듈별로 유일하게 만든다.
 * 예: SPDK_FSDEV_MODULE_REGISTER(aio, ...) → _spdk_fsdev_module_register_aio() 함수가 만들어짐.
 * 컴파일러가 .init_array (또는 .ctors) 섹션에 이 함수의 주소를 넣어, libc 의 _init 단계에서 자동 호출. */

#ifdef __cplusplus
/* [한국어] C++ 링키지 블록 닫기 — 위 extern "C" 와 짝. */
}
#endif

#endif /* SPDK_FSDEV_MODULE_H */
/* [한국어] include guard 닫기 — 라인 5 의 #ifndef 와 짝. */
