/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * File descriptor group utility functions
 */

/*
 * [한국어 설명] epoll 기반 파일 디스크립터 그룹 공개 API (fd_group.h)
 *
 * === 파일의 역할 ===
 * 리눅스의 `epoll(7)` 인스턴스를 SPDK에 맞게 추상화한 **파일 디스크립터 그룹(fgrp)** API를 정의한다.
 * 하나의 `spdk_fd_group`은 내부적으로 epoll_fd 하나를 보유하고, 여기에 사용자 정의 fd를 등록하면
 * 이벤트 발생 시 지정된 콜백(`spdk_fd_fn`)이 호출된다. SPDK의 polled-mode가 **인터럽트 모드**로
 * 전환되거나, NBD/iSCSI/NVMe-oF TCP 등 **커널 fd를 기다려야 하는 경우** 이 모듈이 이벤트 dispatch를
 * 담당한다.
 *
 * 구현은 `lib/util/fd_group.c`에 있으며, 아래 두 종류의 fd를 다룬다:
 *   1. **일반 fd** (socket/eventfd/timerfd/signalfd 등) — epoll에서 레벨 트리거 방식으로 기다림
 *   2. **nested fd_group** (부모 fgrp의 epoll에 자식 fgrp의 epoll_fd를 등록 → 계층적 이벤트 집계)
 *
 * 주요 사용처:
 *   - `lib/thread/`: SPDK thread의 interrupt 모드 — 각 thread가 하나의 fgrp을 소유
 *   - `lib/nbd/`: NBD 소켓 fd 이벤트 감지
 *   - `lib/iscsi/`, `lib/nvmf/tcp/`: TCP 소켓 감지
 *   - `module/bdev/aio/`: aio completion eventfd 감지
 *
 * === 전체 아키텍처에서의 위치 ===
 * **유틸리티 계층(util)** 이지만 실행 모델(reactor/thread) 에 긴밀히 결합된다. SPDK thread는
 * 기본적으로 **polling 모드**(빈 루프)로 동작하지만, CPU 사용률이 문제가 되는 저부하 시나리오에서는
 * `interrupt` 모드로 전환 가능하다. 이때 각 SPDK thread는 자신의 `spdk_fd_group`에서
 * `spdk_fd_group_wait()`를 호출하여 fd 이벤트 발생 시에만 깨어나도록 한다.
 *
 * 호출 체인(인터럽트 모드):
 *   spdk_reactor_run() (interrupt mode)
 *     → spdk_thread가 깨어날 조건 대기
 *       → spdk_fd_group_wait(fgrp, timeout) → epoll_wait(2)
 *         → 이벤트 발생 fd의 spdk_fd_fn(ctx) 콜백 실행
 *           → [사용자 콜백 내부에서 spdk_thread_send_msg 등을 통해 I/O 처리 트리거]
 *
 * 호출 체인(fd 등록 경로):
 *   상위 모듈(nbd/nvme_tcp 등)의 init
 *     → spdk_fd_group_add(fgrp, efd, fn, arg, name) → epoll_ctl(EPOLL_CTL_ADD)
 *
 * === 타 모듈과의 연결 ===
 * - `spdk/stdinc.h`: 기본 타입
 * - `spdk/assert.h`: `SPDK_STATIC_ASSERT` 사용 (opts 구조체 크기 강제)
 * - `<sys/epoll.h>` (리눅스 전용): 내부 구현에서 직접 epoll API 사용. 비-Linux는 구현 stub.
 * - `lib/thread/thread.c`: SPDK thread가 fd_group과 결합되어 interrupt 모드에서 호출 dispatch를 위임.
 *
 * 데이터 흐름:
 *   1. `spdk_fd_group_create(&fgrp)` → 내부 `epoll_create1()` + 관리 자료구조 할당
 *   2. `spdk_fd_group_add(fgrp, efd, fn, arg, name)` → epoll_ctl + 콜백 등록
 *   3. `spdk_fd_group_wait(fgrp, timeout)` → epoll_wait → 각 이벤트에 대해 콜백 실행
 *   4. `spdk_fd_group_event_modify` → 이벤트 비트(EPOLLIN/EPOLLOUT) 동적 변경
 *   5. nest/unnest로 fgrp 계층화 (부모의 wait 한 번으로 자식 이벤트까지 처리)
 *   6. `spdk_fd_group_remove(fgrp, efd)` → epoll_ctl(DEL) + 콜백 해제
 *   7. `spdk_fd_group_destroy(fgrp)` → epoll_fd close + 자료구조 해제
 *
 * === 주요 함수/구조체 요약 ===
 * - `enum spdk_fd_type`: fd 분류 (DEFAULT vs EVENTFD — EVENTFD는 read로 카운터 리셋 필요)
 * - `struct spdk_event_handler_opts`: 등록 시 확장 옵션 (opts_size로 ABI 호환성 보장)
 * - `typedef spdk_fd_fn(ctx)`: 이벤트 콜백. 반환값으로 처리된 이벤트 수를 전달 (0/양수/음수)
 * - `struct spdk_fd_group`: 불투명 구조체. 내부 epoll_fd + 등록된 event source 리스트.
 * - `spdk_fd_group_create/destroy`: 생애주기
 * - `spdk_fd_group_wait(fgrp, timeout)`: **★ 이벤트 루프 진입점** — epoll_wait + dispatch
 * - `spdk_fd_group_add/add_for_events/add_ext`: fd 등록 (세 변형 — 옵션 수준별)
 * - `spdk_fd_group_remove`: fd 해제
 * - `spdk_fd_group_event_modify`: 이벤트 비트 동적 변경 (예: 쓰기 준비 감지 on/off)
 * - `spdk_fd_group_nest/unnest`: **계층적 fgrp** — 부모 wait에서 자식 이벤트도 함께 처리
 * - `spdk_fd_group_get_fd`: 내부 epoll_fd 추출 (다른 epoll에 nest 등록할 때)
 * - `spdk_fd_group_get_epoll_event`: 콜백 내에서만 호출 가능, 발생한 epoll_event 복사
 * - `spdk_fd_group_set_wrapper`: 콜백 dispatch 경로에 래퍼(trace/logging/thread switch 등) 삽입
 * - `SPDK_FD_GROUP_ADD/SPDK_FD_GROUP_ADD_EXT`: 함수명을 자동으로 name 문자열화하는 매크로
 *
 * 경계/주의:
 *   - 모든 API는 **fgrp 소유 스레드에서만** 호출해야 한다 (SPDK thread affinity와 동일 규약).
 *   - `spdk_fd_group_get_epoll_event`는 **콜백 함수 내부에서만** 호출 가능 — 외부에서 부르면 UB.
 *   - `spdk_fd_group_destroy` 전에 모든 event source가 remove되어 있어야 한다.
 *   - nest된 상태에서 자식을 destroy하기 전에 반드시 unnest 호출.
 */

#ifndef SPDK_FD_GROUP_H
#define SPDK_FD_GROUP_H

/* [한국어] SPDK 표준 타입 헤더 포함 — size_t, uint32_t 등. */
#include "spdk/stdinc.h"

#ifdef __cplusplus
/* [한국어] C++ 빌드에서도 C 링키지 유지 (name mangling 방지). */
extern "C" {
#endif

/* [한국어] SPDK_STATIC_ASSERT 매크로 사용을 위해 포함.
 *   - spdk_event_handler_opts 구조체의 크기가 컴파일 시 16 바이트임을 강제하여
 *     ABI 호환성을 검증한다. */
#include "spdk/assert.h"

/**
 * File descriptor type. The event handler may have extra checks and can do extra
 * processing based on this.
 */
/*
 * [한국어] fd의 분류(type) 열거형.
 *   - 이벤트 핸들러(내부 디스패처)가 fd 종류에 따라 **추가 처리**를 수행할 때 쓰인다.
 *   - 예: EVENTFD는 epoll_wait 복귀 후에도 eventfd의 내부 카운터가 남아있어 다음 wait에서
 *     또 깨어나므로, 디스패처가 read(2)로 카운터를 0으로 리셋해야 한다. */
enum spdk_fd_type {
	SPDK_FD_TYPE_DEFAULT		= 0x0,
	/* [한국어] 기본 fd (소켓/timerfd/signalfd 등) — epoll 레벨/엣지 동작에만 의존,
	 *   디스패처가 read 등 추가 작업을 하지 않는다. 콜백이 직접 fd를 처리해야 한다. */

	/**
	 * Event file descriptors. Once an event is generated on these file descriptors, event
	 * handler will perform a read operation on it to reset its internal eventfd object
	 * counter value to 0.
	 */
	SPDK_FD_TYPE_EVENTFD		= 0x1,
	/* [한국어] eventfd(2) 타입 — 콜백 호출 **전**에 디스패처가 `read(efd, &val, 8)`로 카운터를
	 *   0으로 리셋한다. 이렇게 하지 않으면 다음 epoll_wait가 또 깨어나 busy-loop가 발생한다.
	 *   예: SPDK inter-thread notification(spdk_thread_send_msg) 용 eventfd가 이 타입을 사용. */
};

/* [한국어] fd 등록 시 전달하는 **확장 옵션 구조체**.
 *   - 일반 `spdk_fd_group_add`/`add_for_events`와 달리, `add_ext`에서 더 많은 세부 옵션을
 *     지정할 수 있다 (이벤트 타입 + fd 종류).
 *   - **ABI 호환성 패턴**: `opts_size` 필드를 통해 호출자와 라이브러리 간의 구조체 버전 불일치를
 *     허용한다 (새 필드는 반드시 구조체 끝에 추가). */
struct spdk_event_handler_opts {
	/**
	 * The size of spdk_event_handler_opts according to the caller of this library is used for
	 * ABI compatibility. The library uses this field to know how many fields in this structure
	 * are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	/* [한국어] 호출자가 알고 있는 이 구조체의 바이트 크기 (sizeof). 일반적으로
	 *   `sizeof(struct spdk_event_handler_opts)`를 대입.
	 * 설정자: 호출자가 옵션 채울 때 `get_default_event_handler_opts(opts, sizeof(*opts))`로 세팅.
	 * 읽는 자: `spdk_fd_group_add_ext` 내부 — opts_size를 기반으로 "이 호출자가 알고 있는 필드만"
	 *          읽어서 사용. 새 버전의 SPDK가 추가한 필드는 호출자 버전에선 보이지 않아도 기본값으로 대체.
	 * 값 범위: 반드시 sizeof(struct spdk_event_handler_opts) 이하, 0 아님.
	 * 동기화: 구조체는 호출자 스택에 있으므로 통상 동시성 이슈 없음. */

	/** Event notification types */
	uint32_t events;
	/* [한국어] 감시할 이벤트 비트마스크.
	 *   - `SPDK_INTERRUPT_EVENT_IN` (EPOLLIN), `SPDK_INTERRUPT_EVENT_OUT` (EPOLLOUT) 등의 OR 조합.
	 *   - 내부적으로 epoll_event.events 필드에 매핑되어 `epoll_ctl` 호출에 사용된다.
	 * 설정자: 호출자 (예: nbd는 데이터 송신 중에만 EPOLLOUT를 세팅).
	 * 읽는 자: `add_ext` → epoll_ctl(EPOLL_CTL_ADD).
	 * 값 범위: SPDK_INTERRUPT_EVENT_* 비트 조합. 0이면 이 fd는 어떤 이벤트에도 깨어나지 않음 (의미 없음).
	 * 동기화: 등록 후 변경은 `spdk_fd_group_event_modify`를 통해서만 가능. */

	/** fd type \ref spdk_fd_type */
	uint32_t fd_type;
	/* [한국어] fd의 종류 (enum spdk_fd_type). 디스패처가 이벤트 전/후에 추가 처리를 할지 결정.
	 * 설정자: 호출자 — eventfd면 EVENTFD, 그 외 0(DEFAULT).
	 * 읽는 자: 이벤트 디스패처 — EVENTFD이면 콜백 호출 전 read(2)로 카운터 리셋.
	 * 값 범위: enum spdk_fd_type 값. 현재는 DEFAULT(0) 또는 EVENTFD(1).
	 * 동기화: 등록 시 확정, 이후 변경 불가. */
};
/* [한국어] 구조체 크기가 **정확히 16바이트**임을 컴파일 타임에 강제.
 *   - size_t(8) + uint32_t(4) + uint32_t(4) = 16
 *   - 이 크기가 바뀌면 ABI break → 기존 호출자가 잘못된 필드 위치를 읽게 되므로, 새 필드 추가 시
 *     크기 체크도 함께 갱신해야 함을 의도적으로 명시. */
SPDK_STATIC_ASSERT(sizeof(struct spdk_event_handler_opts) == 16, "Incorrect size");

/**
 * Callback function registered for the event source file descriptor.
 *
 * \param ctx Context passed as arg to spdk_fd_group_add().
 *
 * \return 0 to indicate that event notification took place but no events were found;
 * positive to indicate that event notification took place and some events were processed;
 * negative if no event information is provided.
 */
/*
 * [한국어] fd 이벤트 콜백 함수 시그니처.
 *
 * @ctx: `spdk_fd_group_add(... arg ...)` 호출 시 전달한 사용자 컨텍스트 포인터.
 *       콜백 안에서 자신이 어느 모듈/연결을 처리해야 하는지 식별하는 용도.
 *       라이프타임은 호출자 책임 — fd가 등록되어 있는 동안 항상 유효해야 한다.
 *
 * @return:
 *   = 0 : 이벤트 알림이 왔지만 실제로 처리할 이벤트는 없었음 (false-wake / spurious).
 *   > 0 : 처리된 이벤트 수 (통계/스케줄러 힌트로 사용).
 *   < 0 : 처리 정보 없음 — 디스패처가 reentrancy 가드 등을 위해 별도 처리할 수 있음.
 *
 * 실행 컨텍스트: `spdk_fd_group_wait()`를 호출한 SPDK thread 위에서 동기적으로 실행.
 *               콜백이 길어지면 다른 fd의 이벤트 처리가 지연됨. 가능한 한 빨리 반환하고 큰 작업은
 *               메시지로 위임하는 것이 권장.
 * 호출 체인:
 *   spdk_fd_group_wait → epoll_wait → 디스패처 → [spdk_fd_fn] → 사용자 모듈 핸들링
 */
typedef int (*spdk_fd_fn)(void *ctx);

/**
 * A file descriptor group of event sources which gather the events to an epoll instance.
 *
 * Taking "fgrp" as short name for file descriptor group of event sources.
 */
/* [한국어] 파일 디스크립터 그룹 (fgrp) 불투명 전방 선언.
 *   - 내부 정의는 `lib/util/fd_group.c`에 있으며, 다음을 보유:
 *     · int epfd            — epoll_create1로 생성한 epoll 디스크립터
 *     · 등록된 event source 리스트 (각각 efd, fn, arg, name, opts 보관)
 *     · 부모/자식 관계(nest용 포인터)
 *     · 진행 중 wait 추적 등
 *   - "fgrp"는 "file descriptor group"의 약어로 SPDK 코드 전반에서 사용. */
struct spdk_fd_group;

/**
 * Initialize a spdk_event_handler_opts structure to the default values.
 *
 * \param[out] opts Will be filled with default option.
 * \param opts_size Must be the size of spdk_event_handler_opts structure.
 */
/*
 * [한국어]
 * spdk_fd_group_get_default_event_handler_opts - 옵션 구조체를 라이브러리 기본값으로 초기화.
 *
 * @opts: [out] 호출자가 스택/힙에 할당한 옵션 버퍼. 함수 호출 후 기본값으로 채워진다.
 * @opts_size: 호출자가 인식하는 구조체 크기 (sizeof). 라이브러리는 이 크기 안에서만 필드를 채운다.
 *             ABI 호환성: 새 SPDK가 추가한 필드는 이 size를 넘어서면 채우지 않는다.
 *
 * 왜 필요한가: 호출자가 직접 0-init 하면 새 필드가 추가될 때마다 디폴트 값을 알아야 한다.
 *              이 함수가 라이브러리가 권장하는 디폴트(예: events=EPOLLIN, fd_type=DEFAULT)를 채워줘
 *              호출자는 필요한 필드만 덮어쓰면 된다.
 * 동작:
 *   1. opts 구조체를 0으로 memset
 *   2. opts->opts_size = opts_size (caller-provided)
 *   3. 라이브러리 내부 디폴트 값 채움 (events, fd_type 등)
 * 실행 컨텍스트: 어떤 스레드에서나 안전 (스택 변수 초기화).
 * 호출 체인:
 *   사용자 모듈 init → [get_default_event_handler_opts] → 사용자가 일부 필드 변경 → spdk_fd_group_add_ext
 */
void spdk_fd_group_get_default_event_handler_opts(struct spdk_event_handler_opts *opts,
		size_t opts_size);

/**
 * Initialize one fd_group.
 *
 * \param fgrp A pointer to return the initialized fgrp.
 *
 * \return 0 on success, negated errno on failure.
 */
/*
 * [한국어]
 * spdk_fd_group_create - 새 fd_group 인스턴스를 생성하고 epoll 인스턴스를 초기화.
 *
 * @fgrp: [out] 생성된 fgrp 포인터를 받을 이중 포인터. 성공 시 `*fgrp`에 새 인스턴스 주소가 채워진다.
 * @return: 0 = 성공. 음수 errno = 실패 (예: -ENOMEM, -EMFILE — 프로세스 fd 한계 초과 등).
 *
 * 왜 필요한가: 이벤트 디스패처를 사용하려면 우선 fgrp 인스턴스가 필요. 일반적으로 SPDK thread 하나당
 *              하나의 fgrp을 보유한다 (interrupt 모드).
 * 동작:
 *   1. fgrp 구조체 malloc
 *   2. `epoll_create1(EPOLL_CLOEXEC)`로 epoll fd 생성
 *   3. 등록된 이벤트 소스 리스트 초기화 (TAILQ 등)
 *   4. *fgrp = 생성된 인스턴스 주소
 * 에러 경로:
 *   - malloc 실패 → -ENOMEM
 *   - epoll_create1 실패 → -errno (EMFILE/ENFILE 등), 할당된 구조체는 free
 * 실행 컨텍스트: 초기화 경로. fgrp는 이후 호출하는 스레드에 affinity가 묶이게 된다.
 * 호출 체인:
 *   spdk_thread_create (interrupt 모드) → [spdk_fd_group_create] → epoll_create1
 */
int spdk_fd_group_create(struct spdk_fd_group **fgrp);

/**
 * Release all resources associated with this fgrp.
 *
 * Users need to remove all event sources from the fgrp before destroying it.
 *
 * \param fgrp The fgrp to destroy.
 */
/*
 * [한국어]
 * spdk_fd_group_destroy - fgrp 인스턴스 및 내부 epoll fd 해제.
 *
 * @fgrp: 해제할 fgrp. 호출 전에 모든 event source가 `spdk_fd_group_remove`로 제거되어 있어야 함.
 *        남아있는 event source가 있으면 디버그 빌드에서 assert / 릴리스 빌드에서 leak.
 *
 * 왜 필요한가: 인스턴스 종료 — 내부 epoll fd close 및 자료구조 free.
 * 동작:
 *   1. (디버그) 등록된 event source 리스트가 비어있는지 확인
 *   2. (nest 상태이면) 자식이 모두 unnest 되어있는지 확인
 *   3. close(epoll_fd)
 *   4. fgrp 구조체 free
 * 주의: destroy 후 fgrp 포인터는 dangling이 된다. 호출자는 NULL 세팅을 직접 수행해야 함
 *       (다른 SPDK API들과 달리 더블 포인터를 받지 않음 — 시그니처 일관성에서 예외).
 * 실행 컨텍스트: 종료 경로. fgrp 소유 스레드에서 호출.
 * 호출 체인:
 *   spdk_thread_destroy → [spdk_fd_group_destroy] → close + free
 */
void spdk_fd_group_destroy(struct spdk_fd_group *fgrp);

/**
 * Wait for new events generated inside fgrp, and process them with their
 * registered spdk_fd_fn.
 *
 * \param fgrp The fgrp to wait and process.
 * \param timeout Specifies the number of milliseconds that will block.
 * -1 causes indefinitely blocking; 0 causes immediately return.
 *
 * \return the number of events processed on success, negated errno on failure.
 */
/*
 * [한국어]
 * spdk_fd_group_wait - **★ 이벤트 루프의 핵심**: fgrp의 epoll에서 이벤트를 기다리고 콜백을 디스패치.
 *
 * @fgrp: 대상 fgrp.
 * @timeout: epoll_wait의 timeout (ms).
 *           - `-1`: 이벤트가 올 때까지 무한 블로킹 (interrupt 모드의 기본)
 *           - ` 0`: 즉시 반환 (non-blocking poll — busy loop와 결합 시 사용)
 *           - `>0`: 지정 ms만큼만 블로킹 (하이브리드 모드 — polling + interrupt)
 * @return: 처리된 이벤트 수 (>= 0). 음수 errno = 실패 (예: -EINTR — 시그널로 깨어난 경우).
 *
 * 왜 필요한가: SPDK thread가 인터럽트 모드일 때 fd 이벤트를 기다리는 진입점. polling 모드의
 *              `spdk_thread_poll()`에 대응되는 인터럽트 모드 카운터파트.
 * 동작:
 *   1. `epoll_wait(epfd, events_buf, MAX_EVENTS, timeout)` 호출
 *   2. 반환된 각 이벤트에 대해:
 *      a. (옵션) wrapper가 설정되어 있으면 wrapper(cb_fn, cb_ctx) 경유
 *      b. fd_type == EVENTFD이면 `read(efd, &val, 8)`로 카운터 리셋
 *      c. 등록된 spdk_fd_fn(arg) 호출
 *      d. nest된 자식 fgrp의 epfd가 깨면 자식 fgrp의 wait를 재귀 처리
 *   3. 처리된 이벤트 수 합산 후 반환
 * 실행 컨텍스트: fgrp 소유 SPDK thread (single-threaded affinity). 다른 스레드에서 동시 호출 금지.
 * 에러 경로:
 *   - epoll_wait 시스템 호출 실패 (EINTR 등) → 음수 errno 반환, 호출자가 재시도 결정
 *   - 콜백이 음수 반환해도 wait 자체는 성공으로 본다 (개별 콜백 에러는 호출자 모듈이 처리)
 * 호출 체인:
 *   spdk_reactor_run (interrupt mode) → spdk_thread 루프 → [spdk_fd_group_wait]
 *     → epoll_wait → 디스패치 → 사용자 spdk_fd_fn
 */
int spdk_fd_group_wait(struct spdk_fd_group *fgrp, int timeout);

/**
 * Return the internal epoll_fd of specific fd_group
 *
 * \param fgrp The pointer of specified fgrp.
 *
 * \return The epoll_fd of specific fgrp.
 */
/*
 * [한국어]
 * spdk_fd_group_get_fd - fgrp의 내부 epoll fd를 반환 (외부 epoll에 nest 등록할 때 필요).
 *
 * @fgrp: 대상 fgrp.
 * @return: 내부 epoll fd. 항상 유효한 값 (생성 시 epoll_create1로 확보된 값).
 *
 * 왜 필요한가: 부모-자식 fgrp 계층화(`spdk_fd_group_nest`)의 내부 구현 시, **자식의 epoll fd**를
 *              부모의 epoll에 일반 fd처럼 등록해야 한다. 이 추출 함수가 그 다리 역할.
 *              또한 외부 epoll(다른 프레임워크)이 SPDK fgrp의 깨어남을 감지해야 할 때도 사용.
 * 동작: 단순 getter — 내부 epfd 필드 반환.
 * 주의: 반환된 fd를 호출자가 close하면 안 됨 (소유권은 fgrp).
 *       또한 직접 epoll_ctl을 호출해 다른 fd를 추가하면 fgrp의 내부 상태와 어긋난다 — 추가는 반드시
 *       `spdk_fd_group_add` 계열을 통해서만.
 * 실행 컨텍스트: 읽기 전용. 동시성 안전.
 * 호출 체인:
 *   spdk_fd_group_nest → [spdk_fd_group_get_fd] → 부모 epoll에 자식 epfd 등록
 */
int spdk_fd_group_get_fd(struct spdk_fd_group *fgrp);

/**
 * Nest the child fd_group in the parent fd_group. After this operation
 * completes, calling spdk_fd_group_wait() on the parent will include events
 * from the child.
 *
 * \param parent The parent fd_group.
 * \param child The child fd_group.
 *
 * \return 0 on success. Negated errno on failure. However, on all errno values other
 * than -ENOTRECOVERABLE, the operation has not changed the state of the fd_group.
 */
/*
 * [한국어]
 * spdk_fd_group_nest - 자식 fgrp을 부모 fgrp 안에 **계층적으로 중첩**.
 *
 * @parent: 상위 fgrp. 이후 이 fgrp의 wait가 자식 이벤트도 함께 처리.
 * @child:  하위 fgrp. wait는 자체적으로 호출되지 않고, 부모 wait의 디스패치 경로에 통합.
 * @return: 0 = 성공. 음수 errno = 실패.
 *          **트랜잭션성**: -ENOTRECOVERABLE이 아닌 모든 실패는 fgrp 상태를 원래대로 유지.
 *          (즉, 안전한 retry/대체 가능)
 *
 * 왜 필요한가: 여러 모듈이 각자의 fgrp을 가지더라도, 단일 SPDK thread는 wait를 한 번만 부르고
 *              모든 이벤트를 처리하고 싶다. 이 nesting을 통해 트리 구조의 fgrp을 만들어
 *              루트의 wait 한 번으로 전체 트리를 처리할 수 있다.
 * 동작:
 *   1. 자식의 epfd를 추출 (`spdk_fd_group_get_fd(child)`)
 *   2. 부모의 epoll에 자식 epfd를 EPOLLIN으로 등록 — `epoll_ctl(parent->epfd, ADD, child->epfd, ...)`
 *   3. 부모-자식 관계를 메타데이터에 기록
 * 실행 컨텍스트: 두 fgrp이 같은 SPDK thread 소유여야 함 (cross-thread 시 race).
 * 호출 체인:
 *   서브시스템 init → [spdk_fd_group_nest] → epoll_ctl(parent->epfd, ADD, child->epfd)
 */
int spdk_fd_group_nest(struct spdk_fd_group *parent, struct spdk_fd_group *child);

/**
 * Remove the nested child from the parent.
 *
 * \param parent The parent fd_group.
 * \param child The child fd_group.
 *
 * \return 0 on success. Negated errno on failure. However, on all errno values other
 * than -ENOTRECOVERABLE, the operation has not changed the state of the fd_group.
 */
/*
 * [한국어]
 * spdk_fd_group_unnest - 자식 fgrp을 부모에서 분리 (nest의 역연산).
 *
 * @parent: 상위 fgrp.
 * @child:  분리할 자식 fgrp.
 * @return: 0 = 성공. 음수 errno = 실패. -ENOTRECOVERABLE 외의 실패는 상태 보존.
 *
 * 왜 필요한가: 자식 fgrp을 destroy하기 전에 반드시 부모에서 떼어내야 한다. 그렇지 않으면 부모가
 *              dangling epfd를 참조해 epoll_wait에서 EBADF/UB.
 * 동작:
 *   1. `epoll_ctl(parent->epfd, EPOLL_CTL_DEL, child->epfd, NULL)` 호출
 *   2. 부모-자식 메타데이터 정리
 * 실행 컨텍스트: 같은 SPDK thread.
 * 호출 체인:
 *   서브시스템 fini → [spdk_fd_group_unnest] → spdk_fd_group_destroy(child)
 */
int spdk_fd_group_unnest(struct spdk_fd_group *parent, struct spdk_fd_group *child);

/**
 * Register SPDK_INTERRUPT_EVENT_IN event source to specified fgrp.
 *
 * Use spdk_fd_group_add_for_events() for other event types.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 * \param fn Called each time there are events in event source.
 * \param arg Function argument for fn.
 * \param name Name of the event source.
 *
 * \return 0 on success, negated errno on failure.
 */
/*
 * [한국어]
 * spdk_fd_group_add - **읽기(EPOLLIN) 이벤트**용 fd를 fgrp에 등록 (가장 일반적 진입점).
 *
 * @fgrp: 등록 대상 fgrp.
 * @efd:  감시할 파일 디스크립터 (소켓/eventfd/timerfd 등). 호출자가 미리 만들어야 함.
 * @fn:   이벤트 발생 시 호출될 콜백.
 * @arg:  콜백에 전달될 컨텍스트 포인터 (라이프타임은 호출자 책임 — 등록되어 있는 동안 유효해야 함).
 * @name: 디버그/트레이스용 식별 문자열. 보통 콜백 함수명을 그대로 전달 (SPDK_FD_GROUP_ADD 매크로 권장).
 * @return: 0 = 성공. 음수 errno = 실패 (예: -EEXIST, -ENOMEM, -EBADF).
 *
 * 왜 필요한가: 데이터 수신 가능 알림(EPOLLIN)이 가장 흔한 사용 패턴 — 소켓 read 준비, eventfd
 *              알림 등. 이 함수는 events=EPOLLIN, fd_type=DEFAULT의 단축형이다.
 * 동작:
 *   1. 내부적으로 `spdk_fd_group_add_for_events(fgrp, efd, EPOLLIN, fn, arg, name)`과 동등
 *   2. epoll_ctl(EPOLL_CTL_ADD, efd, {events=EPOLLIN, data.ptr=내부 메타})
 *   3. event source 메타데이터(efd, fn, arg, name)를 fgrp 리스트에 추가
 * 실행 컨텍스트: fgrp 소유 SPDK thread.
 * 에러 경로: 같은 efd 중복 등록 시 -EEXIST, malloc 실패 시 -ENOMEM, 잘못된 fd면 -EBADF.
 * 호출 체인:
 *   상위 모듈(nbd/iscsi/aio 등) → [spdk_fd_group_add] → epoll_ctl(ADD)
 */
int spdk_fd_group_add(struct spdk_fd_group *fgrp, int efd,
		      spdk_fd_fn fn, void *arg, const char *name);

/**
 * Register one event source to specified fgrp with specific event types.
 *
 * Event types argument is a bit mask composed by ORing together
 * enum spdk_interrupt_event_types values.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 * \param events Event notification types.
 * \param fn Called each time there are events in event source.
 * \param arg Function argument for fn.
 * \param name Name of the event source.
 *
 * \return 0 on success, negated errno on failure.
 */
/*
 * [한국어]
 * spdk_fd_group_add_for_events - 이벤트 비트마스크를 명시하여 fd 등록 (add보다 세밀한 제어).
 *
 * @fgrp:   등록 대상 fgrp.
 * @efd:    감시할 파일 디스크립터.
 * @events: SPDK_INTERRUPT_EVENT_* 비트 OR 조합.
 *          - EPOLLIN: 읽기 준비 (수신 데이터 도착)
 *          - EPOLLOUT: 쓰기 준비 (송신 버퍼 여유 있음)
 *          - EPOLLERR/EPOLLHUP: 에러/연결 종료 (대부분 자동 포함됨)
 * @fn:     콜백.
 * @arg:    콜백 컨텍스트.
 * @name:   디버그용 이름.
 * @return: 0 = 성공. 음수 errno = 실패.
 *
 * 왜 필요한가: 데이터 송신 가능 시점(EPOLLOUT)도 감지해야 하는 모듈(예: NBD, iSCSI 송신 큐)에서
 *              이벤트 타입을 직접 지정. 보내야 할 데이터가 있을 때만 EPOLLOUT를 켜고, 보낸 뒤에는
 *              `event_modify`로 끄는 패턴이 일반적이다 (불필요한 wakeup 회피).
 * 동작: add와 동일하지만 events 인자를 그대로 epoll_event.events에 사용.
 * 실행 컨텍스트: fgrp 소유 thread.
 * 호출 체인:
 *   nbd/iscsi 송수신 init → [spdk_fd_group_add_for_events] → epoll_ctl(ADD)
 */
int spdk_fd_group_add_for_events(struct spdk_fd_group *fgrp, int efd, uint32_t events,
				 spdk_fd_fn fn, void *arg,  const char *name);

/**
 * Register one event type stated in spdk_event_handler_opts agrument to the specified fgrp.
 *
 * spdk_event_handler_opts argument consists of event which is a bit mask composed by ORing
 * together enum spdk_interrupt_event_types values. It also consists of fd_type, which can be
 * used by event handler to perform extra checks during the spdk_fd_group_wait call.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 * \param fn Called each time there are events in event source.
 * \param arg Function argument for fn.
 * \param name Name of the event source.
 * \param opts Extended event handler option.
 *
 * \return 0 if success or -errno if failed
 */
/*
 * [한국어]
 * spdk_fd_group_add_ext - 확장 옵션(events + fd_type)을 명시하여 fd 등록 (가장 세밀한 진입점).
 *
 * @fgrp:   등록 대상 fgrp.
 * @efd:    파일 디스크립터.
 * @fn:     콜백.
 * @arg:    콜백 컨텍스트.
 * @name:   디버그용 이름.
 * @opts:   확장 옵션 구조체. 호출 전 `get_default_event_handler_opts`로 디폴트 채운 뒤 필드 변경 권장.
 * @return: 0 = 성공, 음수 errno = 실패.
 *
 * 왜 필요한가: fd 종류(EVENTFD)를 지정해야 디스패처가 카운터 자동 리셋을 수행할 수 있음.
 *              SPDK_FD_TYPE_EVENTFD를 명시하지 않으면, eventfd라도 수동 read를 콜백 안에서 해야 한다.
 * 동작:
 *   1. opts->opts_size 검증
 *   2. 내부 event source 메타데이터에 events/fd_type 저장
 *   3. epoll_ctl(EPOLL_CTL_ADD, efd, {events=opts->events, ...})
 * 실행 컨텍스트: fgrp 소유 thread.
 * 호출 체인:
 *   spdk_thread_create의 inter-thread eventfd 등록 → [spdk_fd_group_add_ext]
 */
int spdk_fd_group_add_ext(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn, void *arg,
			  const char *name, struct spdk_event_handler_opts *opts);

/*
 * \brief Register an event source with the name set to the string of the
 * callback function.
 */
/* [한국어] spdk_fd_group_add의 편의 매크로.
 *   - `#fn`: 전처리기 stringification — 콜백 함수명을 문자열로 변환해 name 인자로 자동 전달.
 *   - 디버깅 시 트레이스/로그에서 어떤 핸들러였는지 식별하기 쉬워짐.
 *   - 사용 예: `SPDK_FD_GROUP_ADD(fgrp, sock_fd, _sock_recv_cb, ctx);`
 *     → 내부적으로 name="_sock_recv_cb"로 등록. */
#define SPDK_FD_GROUP_ADD(fgrp, efd, fn, arg) \
	spdk_fd_group_add(fgrp, efd, fn, arg, #fn)

/*
 * \brief Register an event source provided in opts with the name set to the string of the
 * callback function.
 */
/* [한국어] spdk_fd_group_add_ext의 편의 매크로.
 *   - 위 매크로와 동일한 stringification 패턴, 다만 확장 옵션(opts)을 함께 전달.
 *   - 사용 예: `SPDK_FD_GROUP_ADD_EXT(fgrp, efd, my_cb, ctx, &opts);`
 *     → name="my_cb"로 add_ext 호출. */
#define SPDK_FD_GROUP_ADD_EXT(fgrp, efd, fn, arg, opts) \
	spdk_fd_group_add_ext(fgrp, efd, fn, arg, #fn, opts)

/**
 * Unregister one event source from one fgrp.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 */
/*
 * [한국어]
 * spdk_fd_group_remove - 등록된 event source를 fgrp에서 해제 (add의 짝).
 *
 * @fgrp: 대상 fgrp.
 * @efd:  해제할 fd. 등록되어 있지 않으면 디버그 빌드에서 assert / 릴리스 빌드에서 no-op 또는 warn.
 *
 * 왜 필요한가: fd가 close되기 전에 반드시 epoll에서 제거해야 다음 epoll_wait에서 dangling fd
 *              참조로 EBADF/UB가 발생하지 않는다.
 * 동작:
 *   1. event source 메타데이터 검색
 *   2. `epoll_ctl(EPOLL_CTL_DEL, efd, NULL)`
 *   3. 메타데이터 free, 리스트에서 제거
 * 반환값 없음: 실패 케이스를 호출자에게 알리지 않음 — 호출자는 등록 상태를 명확히 알고 호출해야 함.
 * 실행 컨텍스트: fgrp 소유 thread. wait가 진행 중인 콜백 안에서 자기 자신 fd를 remove해도 안전 —
 *               디스패처가 이를 인지하고 다음 이벤트로 진행.
 * 호출 체인:
 *   상위 모듈 fini / 연결 종료 → [spdk_fd_group_remove] → epoll_ctl(DEL)
 */
void spdk_fd_group_remove(struct spdk_fd_group *fgrp, int efd);

/**
 * Change the event notification types associated with the event source.
 *
 * Modules like nbd, need this api to add EPOLLOUT when having data to send, and remove EPOLLOUT if no data to send.
 *
 * \param fgrp The fgrp registered to.
 * \param efd File descriptor of the event source.
 * \param event_types The event notification types.
 *
 * \return 0 on success, negated errno on failure.
 */
/*
 * [한국어]
 * spdk_fd_group_event_modify - 등록된 fd의 이벤트 비트마스크를 동적으로 변경.
 *
 * @fgrp:        대상 fgrp.
 * @efd:         이미 등록된 fd.
 * @event_types: 새 이벤트 비트마스크 (EPOLLIN/EPOLLOUT 조합).
 * @return:      0 = 성공. 음수 errno = 실패 (-ENOENT — 미등록 fd, -EINVAL — 잘못된 비트 등).
 *
 * 왜 필요한가: 보낼 데이터가 있을 때만 EPOLLOUT를 활성화하는 패턴 — 항상 켜두면 송신 버퍼가 비어있는
 *              내내 wakeup이 발생해 CPU를 낭비. 이 동적 토글이 효율적인 송신 큐 모델의 핵심.
 *              예시(nbd):
 *                send_queue가 비어있음   → event_types = EPOLLIN
 *                send_queue에 데이터 있음 → event_types = EPOLLIN|EPOLLOUT
 * 동작: `epoll_ctl(EPOLL_CTL_MOD, efd, {events=event_types, ...})` 호출.
 * 실행 컨텍스트: fgrp 소유 thread.
 * 호출 체인:
 *   nbd 송신 큐 enqueue/완료 → [spdk_fd_group_event_modify] → epoll_ctl(MOD)
 */
int spdk_fd_group_event_modify(struct spdk_fd_group *fgrp,
			       int efd, int event_types);

/*
 * Forward declaration of epoll_event to avoid having to conditionally compile
 * spdk_fd_group_get_epoll_event on non-Linux systems.
 */
/* [한국어] `struct epoll_event`의 전방 선언.
 *   - 비-Linux 환경에서는 <sys/epoll.h>가 없어 `struct epoll_event`도 정의되지 않는다.
 *   - 전방 선언만 두면 `spdk_fd_group_get_epoll_event` 프로토타입은 모든 플랫폼에서 컴파일 가능.
 *   - 실제 구현은 비-Linux에선 stub(예: -ENOTSUP 반환). */
struct epoll_event;

/**
 * Copies the epoll(7) event that caused a callback function to execute.
 * This function can only be called by the callback function, doing otherwise
 * results in undefined behavior.
 *
 * \param event pointer to an epoll(7) event to copy to.
 * \return 0 on success, negated errno on failure.
 */
/*
 * [한국어]
 * spdk_fd_group_get_epoll_event - **콜백 내부에서만** 호출 가능. 현재 디스패치 중인 epoll_event 복사.
 *
 * @event: [out] 호출자가 제공한 epoll_event 버퍼. 함수 호출 후 현재 이벤트 값(events 비트마스크,
 *               data 등)이 복사된다.
 * @return: 0 = 성공. 음수 errno = 실패 (콜백 외에서 호출하면 -EINVAL/UB).
 *
 * 왜 필요한가: 같은 fd를 EPOLLIN+EPOLLOUT로 등록한 경우, 콜백은 어떤 이벤트가 발생했는지 알아야
 *              한다 (수신 처리 vs 송신 처리). 디스패처가 thread-local로 보관해 두는 현재 이벤트를
 *              꺼내볼 수 있게 해주는 함수.
 * 동작:
 *   1. 디스패처가 콜백 호출 직전에 thread-local 변수에 현재 epoll_event를 저장
 *   2. 이 함수는 그 thread-local 값을 호출자 버퍼로 memcpy
 *   3. 콜백 외부에서 호출 시 thread-local이 비어있으므로 UB / -EINVAL
 * 실행 컨텍스트: 반드시 spdk_fd_fn 콜백 내부에서만 호출. 위반 시 UB.
 * 호출 체인:
 *   spdk_fd_fn 내부 → [spdk_fd_group_get_epoll_event] → thread-local read
 */
int spdk_fd_group_get_epoll_event(struct epoll_event *event);

/* [한국어] fd_group dispatch wrapper 함수 시그니처.
 *   - wrapper는 디스패처가 콜백을 호출하기 직전에 끼어들어 추가 동작(트레이싱, lock 잡기,
 *     thread switch 등)을 수행한 뒤 실제 콜백을 실행하는 책임을 진다.
 *   - 반환값은 콜백의 반환값 또는 wrapper 자체의 에러 코드.
 * @wrapper_ctx: set_wrapper 시 등록한 컨텍스트
 * @cb_fn:       원래 등록된 spdk_fd_fn
 * @cb_ctx:      원래 콜백의 arg
 * @return:      cb_fn(cb_ctx)의 반환값 또는 wrapper의 에러
 */
typedef int (*spdk_fd_group_wrapper_fn)(void *wrapper_ctx, spdk_fd_fn cb_fn, void *cb_ctx);

/**
 * Set a wrapper function to be called when an epoll(7) event is received.  The callback associated
 * with that event is passed to the wrapper, which is responsible for executing it.  Only one
 * wrapper can be assigned to an fd_group at a time.
 *
 * \param fgrp fd group.
 * \param cb_fn Wrapper callback.
 * \param cb_ctx Wrapper callback's context.
 *
 * \return 0 on success, negative errno otherwise.
 */
/*
 * [한국어]
 * spdk_fd_group_set_wrapper - 모든 콜백 dispatch에 끼어드는 wrapper 함수를 등록.
 *
 * @fgrp:   대상 fgrp.
 * @cb_fn:  새 wrapper 함수. NULL을 전달하면 wrapper 해제 (구현에 따라 다름 — 코드 확인 필요).
 * @cb_ctx: wrapper에 전달될 컨텍스트.
 * @return: 0 = 성공. 음수 errno = 실패.
 *
 * 왜 필요한가:
 *   - 트레이싱: 모든 콜백 진입/탈출 시점을 기록
 *   - SPDK thread switch: 콜백 실행 전에 spdk_thread context를 활성화
 *   - 추가 락/체크
 * 한 번에 하나만 등록 가능 — 두 번째 호출은 첫 번째를 덮어쓴다.
 * 동작: fgrp 내부에 wrapper 함수 포인터와 컨텍스트를 저장. 이후 디스패처는 모든 콜백 호출을
 *       `wrapper(wrapper_ctx, cb_fn, cb_ctx)` 형태로 변환.
 * 실행 컨텍스트: 보통 init 시 한 번만 호출. fgrp 소유 thread.
 * 호출 체인:
 *   spdk_thread init → [spdk_fd_group_set_wrapper] → 이후 모든 wait의 dispatch가 wrapper 경유
 */
int spdk_fd_group_set_wrapper(struct spdk_fd_group *fgrp, spdk_fd_group_wrapper_fn cb_fn,
			      void *cb_ctx);

#ifdef __cplusplus
/* [한국어] C++ extern "C" 블록 닫기. */
}
#endif

#endif /* SPDK_FD_GROUP_H */
/* [한국어] SPDK_FD_GROUP_H 헤더 가드 종료. */
