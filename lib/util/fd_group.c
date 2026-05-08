/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

/*
 * [한국어 설명] epoll 기반 fd 그룹화 및 인터럽트 모드 폴러 다중화 (fd_group.c)
 *
 * === 파일의 역할 ===
 * SPDK의 "interrupt mode"(인터럽트 기반 폴링) 지원을 위한 fd 다중화 메커니즘이다.
 * 본래 SPDK는 polled-mode가 기본이지만, 전력 소모/저부하 환경을 위한 인터럽트 모드에서
 * spdk_thread/poller가 다수의 fd(eventfd, timerfd, 일반 socket fd 등)에서의 이벤트
 * 발생을 epoll로 한꺼번에 기다린다. 이 파일은 fd 그룹(=epoll instance)의 생성/소멸,
 * fd 등록/제거, 부모-자식 그룹 nesting/unnesting, 그리고 wait 루프(=epoll_wait)에서
 * 발생한 이벤트들을 등록 콜백(spdk_fd_fn)으로 디스패치한다. nesting이 가능한 이유는
 * 하나의 reactor가 여러 thread의 fd 집합을 한 epoll 인스턴스로 통합 wait하기 위함.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK util 라이브러리 → 인터럽트 모드 fd 다중화.
 * 호출 체인:
 *   spdk_thread (interrupt mode) / spdk_reactor 인터럽트 루프
 *     → spdk_fd_group_wait (이 파일) → epoll_wait → ehdlr->fn(arg) (사용자 콜백)
 *   spdk_fd_group_add: fd를 그룹에 등록 → epoll_ctl(EPOLL_CTL_ADD)
 *   spdk_fd_group_nest: 자식 그룹의 모든 fd를 부모의 epfd로 이동(루트만 epoll_wait).
 * 실행 컨텍스트: 호스트 유저스페이스, Linux 전용(__linux__). non-Linux 빌드에서는
 * 모든 함수가 -ENOTSUP를 반환하는 stub로 컴파일된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk_internal/usdt.h(USDT 정적 추적), spdk/env.h, spdk/log.h(SPDK_ERRLOG/WARNLOG),
 *   spdk/queue.h(TAILQ_*), spdk/util.h, spdk/fd_group.h(공개 API).
 * - 의존하는 자: lib/thread/* (interrupt mode thread/reactor), lib/event/* (reactor 루프),
 *   nbd/sock 모듈 등 인터럽트 가능 fd를 가진 모든 SPDK 컴포넌트.
 * - 데이터 흐름: 사용자 fd + 콜백 fn → event_handler 노드 생성 → 부모 그룹의 epfd에 등록 →
 *   reactor가 epoll_wait → 발생 이벤트의 ptr로 event_handler 복원 → fn(arg) 호출.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct event_handler        : fd 하나당 등록 정보(콜백 fn/arg, fd, 이벤트 마스크, 상태).
 * - struct spdk_fd_group        : epoll 인스턴스 + event_handler 리스트 + 자식 그룹.
 * - spdk_fd_group_create/destroy: 그룹 생성/해제(epoll_create1 / close).
 * - spdk_fd_group_add[_ext]     : fd 등록.
 * - spdk_fd_group_remove        : fd 제거(이벤트 디스패치 중이면 RUNNING/REMOVED 지연 해제).
 * - spdk_fd_group_event_modify  : 등록된 fd의 epoll 이벤트 마스크 변경.
 * - spdk_fd_group_nest/unnest   : 부모-자식 그룹 합성/분리(자식의 모든 fd를 루트로 이동).
 * - spdk_fd_group_wait          : epoll_wait + 이벤트 디스패치(이 파일의 핵심).
 * - spdk_fd_group_set_wrapper   : 콜백 호출을 감싸는 wrapper(예: SPDK thread 컨텍스트로 전환).
 *
 * === 핵심 설계 고려 ===
 * 1) 이벤트 디스패치 도중에 콜백이 같은 fd를 remove할 수 있다. 이를 안전히 처리하기 위해
 *    event_handler.state(WAITING/RUNNING/REMOVED)로 상태 머신을 관리한다.
 *    RUNNING 중에 remove 요청이 오면 state를 REMOVED로 마킹만 하고 free는 wait 루프 끝에서.
 * 2) nest된 자식 그룹의 fd는 루트의 epfd로 이동된다. 자식 그룹의 epfd는 비어있는 상태로
 *    유지되며, spdk_fd_group_wait를 자식에 직접 호출하면 무한 블록 위험을 막기 위해 경고/에러.
 * 3) g_event는 thread-local 포인터로, 콜백 안에서 spdk_fd_group_get_epoll_event를 호출하면
 *    현재 처리 중인 epoll_event를 조회 가능(예: 어떤 이벤트 비트가 켜졌는지 확인용).
 */

#include "spdk_internal/usdt.h"
/* [한국어] USDT(User Statically-Defined Tracing) 매크로. SystemTap/perf로 트레이싱 가능한 마커. */

#include "spdk/env.h"
/* [한국어] DPDK 환경 추상화(이 파일에서는 직접 사용 안 하지만 의존성 그래프 통일). */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/WARNLOG 매크로. */
#include "spdk/queue.h"
/* [한국어] TAILQ_ENTRY/TAILQ_HEAD/TAILQ_FOREACH 등 BSD 스타일 큐 자료구조. */
#include "spdk/util.h"
/* [한국어] 잡다한 유틸 매크로(이 파일에서는 SPDK_STATIC_ASSERT 등). */

#include "spdk/fd_group.h"
/* [한국어] 공개 API와 spdk_event_handler_opts 구조체 정의. */

#define SPDK_MAX_EVENT_NAME_LEN 256
/* [한국어] event_handler.name 버퍼 최대 길이. 디버그/로그/추적용 식별자. */

/*
 * [한국어] event_handler 상태 머신 - wait 루프와 remove의 race를 안전히 처리하기 위함.
 * 한 SPDK thread 안에서만 전환되므로 락이 필요 없다.
 */
enum event_handler_state {
	/* The event_handler is added into an fd_group waiting for event,
	 * but not currently in the execution of a wait loop.
	 */
	EVENT_HANDLER_STATE_WAITING,
	/* [한국어] 등록되어 있고 wait 루프에 진입하지 않은 정적인 상태(보통 이 상태). */

	/* The event_handler is currently in the execution of a wait loop. */
	EVENT_HANDLER_STATE_RUNNING,
	/* [한국어] 현재 spdk_fd_group_wait에서 콜백 fn이 디스패치 중. 이 동안 remove되면 REMOVED로. */

	/* The event_handler was removed during the execution of a wait loop. */
	EVENT_HANDLER_STATE_REMOVED,
	/* [한국어] RUNNING 도중 remove가 호출되어 free가 지연된 상태. wait 루프가 끝에서 free. */
};

/* Taking "ehdlr" as short name for file descriptor handler of the interrupt event. */
/* [한국어] event_handler: fd 하나당 등록되는 콜백 노드. fd_group의 큐에 연결. */
struct event_handler {
	TAILQ_ENTRY(event_handler)	next;
	/* [한국어] fd_group의 event_handlers TAILQ 연결 노드.
	 * 설정자: TAILQ_INSERT_TAIL/REMOVE.
	 * 동기화: 같은 SPDK thread 내에서만 조작. */
	enum event_handler_state	state;
	/* [한국어] 위 enum의 현재 상태(WAITING/RUNNING/REMOVED).
	 * 설정자/읽는 자: spdk_fd_group_wait, spdk_fd_group_remove.
	 * 동기화: 단일 스레드 내 사용. */

	spdk_fd_fn			fn;
	/* [한국어] fd에 이벤트 발생 시 호출될 사용자 콜백 함수.
	 * 설정자: spdk_fd_group_add_ext.
	 * 호출자: spdk_fd_group_wait의 디스패치 루프. */
	void				*fn_arg;
	/* [한국어] 콜백에 전달되는 사용자 컨텍스트 포인터. 보통 모듈별 객체. */
	/* file descriptor of the interrupt event */
	int				fd;
	/* [한국어] 모니터링 대상 fd(eventfd/timerfd/socket 등).
	 * 동기화: 등록 후 변경되지 않음. close/duplicate는 호출자 책임. */
	uint32_t			events;
	/* [한국어] epoll 이벤트 마스크(EPOLLIN | EPOLLOUT | ...).
	 * 설정자: add_ext 시 또는 event_modify로 변경 가능. */
	uint32_t			fd_type;
	/* [한국어] fd의 종류 표시(SPDK_FD_TYPE_DEFAULT, SPDK_FD_TYPE_EVENTFD 등).
	 * EVENTFD인 경우 wait 루프가 자동으로 read()로 카운터를 0으로 리셋. */
	struct spdk_fd_group		*owner;
	/* [한국어] 이 핸들러를 등록한 fd_group(소유자). nest/unnest 시에도 owner는 자신의 그룹.
	 * wrapper_fn 적용 여부 결정에 사용. */
	char				name[SPDK_MAX_EVENT_NAME_LEN + 1];
	/* [한국어] 디버그/로그용 이름. snprintf로 truncate 보장 + 1바이트 NUL. */
};

/*
 * [한국어] spdk_fd_group: epoll 인스턴스 + event_handler 리스트 + 자식 그룹.
 * 한 reactor의 인터럽트 모드 wait 단위. nesting 시 루트가 모든 자식 fd를 통합 보유.
 */
struct spdk_fd_group {
	int epfd;
	/* [한국어] epoll 인스턴스 fd. epoll_create1(EPOLL_CLOEXEC)로 생성, destroy에서 close.
	 * nesting된 자식의 epfd는 비어있는 상태로 유지(실제 fd는 루트의 epfd에 등록됨). */

	/* Number of fds registered in this group. The epoll file descriptor of this fd group
	 * i.e. epfd waits for interrupt event on all the fds from its interrupt sources list, as
	 * well as from all its children fd group interrupt sources list.
	 */
	uint32_t num_fds;
	/* [한국어] 이 그룹(루트인 경우 자식까지 포함)에 등록된 fd 총 개수.
	 * 설정자: add/remove에서 ++/--, nest/unnest에서 일괄 +=/-=.
	 * 읽는 자: spdk_fd_group_wait가 events 배열 크기 결정에 사용. */

	struct spdk_fd_group *parent;
	/* [한국어] 부모 그룹 포인터. NULL이면 루트.
	 * 설정자: nest/unnest. */
	spdk_fd_group_wrapper_fn wrapper_fn;
	/* [한국어] 콜백 호출을 감싸는 함수(예: SPDK thread 컨텍스트 진입/탈출 처리).
	 * NULL이면 콜백을 직접 호출. */
	void *wrapper_arg;
	/* [한국어] wrapper_fn에 전달되는 첫 인자. */

	/* interrupt sources list */
	TAILQ_HEAD(, event_handler) event_handlers;
	/* [한국어] 이 그룹에 등록된 event_handler들의 리스트(자식 그룹의 것은 자식이 보유). */
	TAILQ_HEAD(, spdk_fd_group) children;
	/* [한국어] 자식 fd_group 리스트(자식이 nest로 추가됨). */
	TAILQ_ENTRY(spdk_fd_group) link;
	/* [한국어] 부모의 children 리스트에 연결되는 노드. */
};

/*
 * [한국어]
 * spdk_fd_group_get_fd - 그룹의 epoll fd를 외부에 노출.
 *
 * 다른 epoll 인스턴스에 이 fd를 등록하여 더 큰 다중화 트리를 만들고 싶을 때 사용.
 */
int
spdk_fd_group_get_fd(struct spdk_fd_group *fgrp)
{
	return fgrp->epfd;
	/* [한국어] 단순 필드 반환. */
}

#ifdef __linux__
/* [한국어] Linux 빌드: epoll API를 사용한 실제 구현. 다른 OS는 파일 끝의 stub 사용. */

static __thread struct epoll_event *g_event = NULL;
/* [한국어] thread-local 포인터: 현재 디스패치 중인 epoll_event를 가리킨다.
 * 설정자: spdk_fd_group_wait 디스패치 루프가 매 콜백 호출 직전에 갱신.
 * 읽는 자: spdk_fd_group_get_epoll_event(콜백 내부에서).
 * 동기화: __thread 키워드로 스레드별 독립 변수 - 락 불필요. */

/*
 * [한국어]
 * spdk_fd_group_get_epoll_event - 현재 콜백 컨텍스트의 epoll_event를 조회.
 *
 * 콜백이 어떤 이벤트 비트(EPOLLIN/EPOLLERR 등)로 깨어났는지 확인할 때 사용.
 * 콜백이 아닌 곳에서 호출하면 -EINVAL.
 */
int
spdk_fd_group_get_epoll_event(struct epoll_event *event)
{
	if (g_event == NULL) {
		/* [한국어] wait 디스패치 외 컨텍스트 - 호출이 잘못됨. */
		return -EINVAL;
	}
	*event = *g_event;
	/* [한국어] 호출자 버퍼에 현재 epoll_event 사본 복사. */
	return 0;
}

/*
 * [한국어]
 * _fd_group_del_all - grp의 모든 fd를 epfd에서 제거(EPOLL_CTL_DEL).
 *
 * @epfd: 대상 epoll fd(보통 nest 변경 시 기존 부모의 epfd).
 * @grp:  fd 목록 소유 그룹.
 * @return: 성공 시 제거한 fd 개수, 실패 시 -errno (recover 후 -ENOTRECOVERABLE 가능).
 *
 * nest 변경의 일부로 사용. 실패 시 모든 fd를 다시 추가해 원상 복구를 시도.
 */
static int
_fd_group_del_all(int epfd, struct spdk_fd_group *grp)
{
	struct event_handler *ehdlr = NULL;
	/* [한국어] 순회용 핸들러 포인터. */
	struct epoll_event epevent = {0};
	/* [한국어] 복구 시 추가 호출에 사용할 버퍼. */
	int rc;
	int ret = 0;
	/* [한국어] ret: 누적된 결과(성공 시 +1씩, 실패 시 음수 errno). */

	TAILQ_FOREACH(ehdlr, &grp->event_handlers, next) {
		rc = epoll_ctl(epfd, EPOLL_CTL_DEL, ehdlr->fd, NULL);
		/* [한국어] 각 fd를 epoll에서 제거. NULL은 EPOLL_CTL_DEL에 무관(ignore). */
		if (rc < 0) {
			if (errno == ENOENT) {
				/* This is treated as success. It happens if there are multiple
				 * attempts to remove fds from the group.
				 */
				/* [한국어] 이미 제거된 fd - 멱등 처리. */
				continue;
			}

			ret = -errno;
			SPDK_ERRLOG("Failed to remove fd: %d from group: %s\n",
				    ehdlr->fd, strerror(errno));
			goto recover;
			/* [한국어] 다른 에러는 복구 경로로 점프. */
		}
		ret++;
		/* [한국어] 성공한 제거 카운트 증가. */
	}

	return ret;

recover:
	/* We failed to remove everything. Let's try to put everything back into
	 * the original group. */
	TAILQ_FOREACH(ehdlr, &grp->event_handlers, next) {
		epevent.events = ehdlr->events;
		/* [한국어] 원래 마스크로 복원 준비. */
		epevent.data.ptr = ehdlr;
		/* [한국어] data.ptr에 ehdlr 복원 - 이벤트 시 wait가 ehdlr 식별 가능. */
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, ehdlr->fd, &epevent);
		/* [한국어] 다시 추가하여 원상 복구 시도. */
		if (rc < 0) {
			if (errno == EEXIST) {
				/* This is fine. Keep going. */
				/* [한국어] 이미 존재하면 OK - 부분 제거 상태에서의 잔여 항목. */
				continue;
			}

			/* Continue on even though we've failed. But indicate
			 * this is a fatal error. */
			SPDK_ERRLOG("Failed to recover fd_group_del_all: %s\n", strerror(errno));
			ret = -ENOTRECOVERABLE;
			/* [한국어] 복구 자체 실패 - 시스템 상태 일관성이 깨졌을 가능성. */
		}
	}

	return ret;
}

/*
 * [한국어]
 * _fd_group_add_all - grp의 모든 fd를 epfd에 추가(EPOLL_CTL_ADD).
 *
 * @epfd: 새 부모의 epoll fd.
 * @grp:  fd 소스 그룹.
 * @return: 성공 시 추가한 fd 수, 실패 시 -errno (recover 후 -ENOTRECOVERABLE 가능).
 *
 * nest 시 자식 fd를 부모 epfd로 끌어올리는 용도. 실패 시 추가한 fd를 되돌리는 복구 수행.
 */
static int
_fd_group_add_all(int epfd, struct spdk_fd_group *grp)
{
	struct event_handler *ehdlr = NULL;
	struct epoll_event epevent = {0};
	int rc;
	int ret = 0;

	/* Hoist the fds from the child up into the parent */
	TAILQ_FOREACH(ehdlr, &grp->event_handlers, next) {
		epevent.events = ehdlr->events;
		/* [한국어] 핸들러 등록 시 지정된 이벤트 마스크 설정. */
		epevent.data.ptr = ehdlr;
		/* [한국어] wait가 ehdlr를 복원할 수 있도록 포인터 부착. */
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, ehdlr->fd, &epevent);
		/* [한국어] 새 epoll 인스턴스에 등록. */
		if (rc < 0) {
			if (errno == EEXIST) {
				/* This is treated as success */
				/* [한국어] 이미 등록된 경우 - 멱등 성공. */
				continue;
			}

			ret = -errno;
			SPDK_ERRLOG("Failed to add fd: %d to fd group: %s\n",
				    ehdlr->fd, strerror(errno));
			goto recover;
		}
		ret++;
		/* [한국어] 성공 카운트. */
	}

	return ret;

recover:
	/* We failed to add everything, so try to remove what we did add. */
	TAILQ_FOREACH(ehdlr, &grp->event_handlers, next) {
		rc = epoll_ctl(epfd, EPOLL_CTL_DEL, ehdlr->fd, NULL);
		/* [한국어] 추가했던 항목 제거 시도. */
		if (rc < 0) {
			if (errno == ENOENT) {
				/* This is treated as success. */
				/* [한국어] 이미 없으면 OK. */
				continue;
			}


			/* Continue on even though we've failed. But indicate
			 * this is a fatal error. */
			SPDK_ERRLOG("Failed to recover fd_group_del_all: %s\n", strerror(errno));
			ret = -ENOTRECOVERABLE;
			/* [한국어] 복구 실패 - 일관성 손상 가능. */
		}
	}

	return ret;
}

/*
 * [한국어]
 * fd_group_get_root - 그룹 트리에서 루트(최상위 부모)를 찾는다.
 *
 * 루트는 epoll_wait를 실행하는 그룹이며 모든 자손 fd를 자신의 epfd에 보유.
 */
static struct spdk_fd_group *
fd_group_get_root(struct spdk_fd_group *fgrp)
{
	while (fgrp->parent != NULL) {
		fgrp = fgrp->parent;
		/* [한국어] 부모를 따라 올라감. */
	}

	return fgrp;
	/* [한국어] parent==NULL인 노드가 루트. */
}

/*
 * [한국어]
 * fd_group_change_parent - fgrp(과 모든 자손)의 fd 등록을 old의 epfd에서 new의 epfd로 이전.
 *
 * @fgrp:   이동 대상 서브트리 루트.
 * @old:    기존 부모(epoll 인스턴스).
 * @new:    새 부모.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 동작: 자손부터 재귀적으로 처리(post-order) → 자기 fd들 _del_all/_add_all 수행.
 * 실패 시 단계별 recover_children/recover_epfd로 원상 복귀 시도.
 * 호출처: spdk_fd_group_nest/unnest.
 */
static int
fd_group_change_parent(struct spdk_fd_group *fgrp, struct spdk_fd_group *old,
		       struct spdk_fd_group *new)
{
	struct spdk_fd_group *child, *tmp;
	int rc, ret;

	TAILQ_FOREACH(child, &fgrp->children, link) {
		ret = fd_group_change_parent(child, old, new);
		/* [한국어] 자손부터 재귀 처리(자식이 옮겨가야 자기 fd들도 안전히 옮길 수 있음). */
		if (ret != 0) {
			goto recover_children;
		}
	}

	ret = _fd_group_del_all(old->epfd, fgrp);
	/* [한국어] 자기 그룹의 fd들을 기존 epfd에서 제거. ret = 제거된 개수 또는 -errno. */
	if (ret < 0) {
		goto recover_children;
	}

	assert(old->num_fds >= (uint32_t)ret);
	old->num_fds -= ret;
	/* [한국어] 기존 부모 카운트 갱신. */

	ret = _fd_group_add_all(new->epfd, fgrp);
	/* [한국어] 새 epfd에 동일 fd들을 추가. */
	if (ret < 0) {
		goto recover_epfd;
	}

	new->num_fds += ret;
	/* [한국어] 새 부모 카운트 갱신. */
	return 0;

recover_epfd:
	if (ret == -ENOTRECOVERABLE) {
		/* [한국어] add_all 도중 recover 실패 → 자식까지 일관성 손상. 자식 복구로 점프. */
		goto recover_children;
	}
	rc = _fd_group_add_all(old->epfd, fgrp);
	/* [한국어] del_all로 옮긴 fd들을 다시 old로 복귀. */
	if (rc >= 0) {
		old->num_fds += rc;
		/* [한국어] 복구 성공 - 카운트 원복. */
	} else {
		SPDK_ERRLOG("Failed to recover epfd\n");
		ret = -ENOTRECOVERABLE;
		/* [한국어] 복구도 실패 - 시스템 일관성 깨짐. */
	}
recover_children:
	TAILQ_FOREACH(tmp, &fgrp->children, link) {
		if (tmp == child) {
			/* [한국어] 실패 직전까지 처리한 자식들만 되돌리고 중단(부분 진행 보호). */
			break;
		}
		rc = fd_group_change_parent(tmp, new, old);
		/* [한국어] 자식들을 역방향으로 다시 이전. */
		if (rc != 0) {
			SPDK_ERRLOG("Failed to recover fd_group_change_parent\n");
			ret = -ENOTRECOVERABLE;
		}
	}
	return ret;
}

/*
 * [한국어]
 * spdk_fd_group_unnest - child를 parent의 자식 관계에서 분리하고 독립 그룹으로 복귀.
 *
 * @parent: 현재 부모.
 * @child:  분리할 자식.
 * @return: 0 성공, -EINVAL 인자 오류 또는 관계 불일치, fd 이전 실패 시 그 errno.
 *
 * 자식의 모든 fd를 루트 epfd에서 제거 후 자식 자신의 epfd에 다시 등록.
 */
int
spdk_fd_group_unnest(struct spdk_fd_group *parent, struct spdk_fd_group *child)
{
	struct spdk_fd_group *root;
	int rc;

	if (parent == NULL || child == NULL) {
		/* [한국어] NULL 인자 거부. */
		return -EINVAL;
	}

	if (child->parent != parent) {
		/* [한국어] child가 실제로 parent의 자식이 아니면 거부. */
		return -EINVAL;
	}

	root = fd_group_get_root(parent);
	assert(root == parent || parent->num_fds == 0);
	/* [한국어] 비-루트 부모는 자체 fd가 없어야 함(트리 불변). */

	rc = fd_group_change_parent(child, root, child);
	/* [한국어] child의 모든 fd를 root->epfd에서 child->epfd로 이전. */
	if (rc != 0) {
		return rc;
	}

	child->parent = NULL;
	/* [한국어] 부모 관계 제거 - 이제 child가 자기 epfd로 독립적으로 wait 가능. */
	TAILQ_REMOVE(&parent->children, child, link);
	/* [한국어] 부모의 children 리스트에서 제거. */

	return 0;
}

/*
 * [한국어]
 * spdk_fd_group_nest - child를 parent의 자식으로 합성. child의 모든 fd를 root epfd로 이동.
 *
 * @return: 0 성공, -EINVAL 인자 오류/이미 부모 있음/parent에 wrapper_fn 있음.
 *
 * 제약: parent에 wrapper_fn이 있으면 nest 불가(콜백 컨텍스트 처리 충돌 회피).
 */
int
spdk_fd_group_nest(struct spdk_fd_group *parent, struct spdk_fd_group *child)
{
	struct spdk_fd_group *root;
	int rc;

	if (parent == NULL || child == NULL) {
		/* [한국어] NULL 거부. */
		return -EINVAL;
	}

	if (child->parent) {
		/* [한국어] 이미 다른 부모에 nest되어 있으면 거부 - 트리 불변 유지. */
		return -EINVAL;
	}

	if (parent->wrapper_fn != NULL) {
		/* [한국어] wrapper가 있는 parent는 자식 콜백 컨텍스트와 충돌 → 거부. */
		return -EINVAL;
	}

	/* The epoll instance at the root holds all fds, so either the parent is the root or it
	 * doesn't hold any fds.
	 */
	root = fd_group_get_root(parent);
	assert(root == parent || parent->num_fds == 0);
	/* [한국어] 루트만 fd를 보유하는 트리 불변 검증. */

	rc = fd_group_change_parent(child, child, root);
	/* [한국어] child의 모든 fd를 자체 epfd → root epfd로 이동. */
	if (rc != 0) {
		return rc;
	}

	child->parent = parent;
	/* [한국어] 부모 관계 설정. */
	TAILQ_INSERT_TAIL(&parent->children, child, link);
	/* [한국어] 부모의 children 리스트에 추가. */

	return 0;
}

/*
 * [한국어]
 * spdk_fd_group_get_default_event_handler_opts - opts 구조체에 기본값을 채운다.
 *
 * @opts:      [out] 채울 버퍼.
 * @opts_size: 호출자가 가진 구조체 크기(ABI 진화 호환을 위한 size 패턴).
 *
 * SPDK는 옵션 구조체에 opts_size 필드를 두고 호출자가 자기가 컴파일한 크기를 전달한다.
 * 이 함수는 이 크기 안에서 안전하게 필드를 0초기화하고 기본값을 채워 넣는다.
 * 새 필드 추가 시에도 구버전 호출자가 새 필드를 모르더라도 ABI가 깨지지 않게 한다.
 */
void
spdk_fd_group_get_default_event_handler_opts(struct spdk_event_handler_opts *opts,
		size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	memset(opts, 0, opts_size);
	/* [한국어] 호출자 크기만큼 0 초기화 - 새 필드들은 모두 0(미설정 의미). */
	opts->opts_size = opts_size;
	/* [한국어] 자신의 크기를 기록(이후 copy/검증에서 사용). */

#define FIELD_OK(field) \
        offsetof(struct spdk_event_handler_opts, field) + sizeof(opts->field) <= opts_size
/* [한국어] 매크로: field가 호출자 opts_size 안에 들어가는지 검사 - ABI 호환 보장. */

#define SET_FIELD(field, value) \
        if (FIELD_OK(field)) { \
                opts->field = value; \
        } \
/* [한국어] 매크로: 필드가 안전히 들어가는 경우에만 값 설정. */

	SET_FIELD(events, EPOLLIN);
	/* [한국어] 기본 이벤트 마스크 = 읽기 가능(EPOLLIN). 가장 흔한 사용 케이스. */
	SET_FIELD(fd_type, SPDK_FD_TYPE_DEFAULT);
	/* [한국어] 기본 fd 종류 = 일반 fd(socket/timerfd 등). EVENTFD는 호출자가 명시적으로 설정. */

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * event_handler_opts_copy - src opts를 dst로 ABI 호환적으로 복사.
 *
 * 호출자가 다른 SPDK 버전에서 만든 opts(=다른 opts_size)를 그대로 복사할 때 안전.
 */
static void
event_handler_opts_copy(const struct spdk_event_handler_opts *src,
			struct spdk_event_handler_opts *dst)
{
	if (!src->opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		assert(false);
		/* [한국어] opts_size 0은 사용자 버그 - 디버그 빌드에서 즉시 중단. */
	}

#define FIELD_OK(field) \
        offsetof(struct spdk_event_handler_opts, field) + sizeof(src->field) <= src->opts_size
/* [한국어] 매크로: field가 src->opts_size 안에 들어가는지 검사. */

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \
/* [한국어] 매크로: src에 정의된 경우에만 dst로 복사. 미정의(구버전)는 dst의 기존 값 유지. */

	SET_FIELD(events);
	/* [한국어] 이벤트 마스크 복사. */
	SET_FIELD(fd_type);
	/* [한국어] fd 종류 복사. */

	dst->opts_size = src->opts_size;
	/* [한국어] dst의 size 필드는 src 그대로 - 이후 비교 일관성 유지. */

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_event_handler_opts) == 16, "Incorrect size");
	/* [한국어] 구조체 크기를 16B로 고정 검증. 필드 추가 시 컴파일 에러로 SET_FIELD 추가 잊지 않게. */

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * spdk_fd_group_add - 기본 옵션(EPOLLIN, DEFAULT 타입)으로 fd 등록.
 */
int
spdk_fd_group_add(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn,
		  void *arg, const char *name)
{
	return spdk_fd_group_add_for_events(fgrp, efd, EPOLLIN, fn, arg, name);
	/* [한국어] EPOLLIN(읽기 가능 이벤트)을 기본으로 위임. */
}

/*
 * [한국어]
 * spdk_fd_group_add_for_events - 사용자 지정 이벤트 마스크로 등록.
 */
int
spdk_fd_group_add_for_events(struct spdk_fd_group *fgrp, int efd, uint32_t events,
			     spdk_fd_fn fn, void *arg, const char *name)
{
	struct spdk_event_handler_opts opts = {};
	/* [한국어] 옵션 구조체 zero-initialize. */

	spdk_fd_group_get_default_event_handler_opts(&opts, sizeof(opts));
	/* [한국어] 기본값으로 채우고. */
	opts.events = events;
	/* [한국어] 호출자 지정 이벤트 마스크 적용. */
	opts.fd_type = SPDK_FD_TYPE_DEFAULT;
	/* [한국어] fd_type은 기본 - 이 함수는 일반 fd 가정. */

	return spdk_fd_group_add_ext(fgrp, efd, fn, arg, name, &opts);
	/* [한국어] 풀 옵션 API에 위임. */
}

/*
 * [한국어]
 * spdk_fd_group_add_ext - fd 등록 (가장 일반적인 형태).
 *
 * @fgrp:  대상 그룹.
 * @efd:   모니터링할 fd.
 * @fn:    이벤트 발생 시 호출될 콜백.
 * @arg:   콜백 컨텍스트.
 * @name:  디버그용 이름.
 * @opts:  옵션(이벤트 마스크, fd_type). NULL이면 기본값 사용.
 * @return: 0 성공, -EINVAL/-EEXIST/-errno 실패.
 *
 * 동작: event_handler 노드 생성 → 루트 epfd에 epoll_ctl(ADD) → 그룹 리스트에 연결.
 * fgrp가 nest된 자식이면 실제 fd는 root->epfd에 등록된다.
 */
int
spdk_fd_group_add_ext(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn, void *arg,
		      const char *name, struct spdk_event_handler_opts *opts)
{
	struct event_handler *ehdlr = NULL;
	/* [한국어] 새로 만들 핸들러 노드. */
	struct epoll_event epevent = {0};
	/* [한국어] epoll_ctl에 전달할 이벤트 명세. */
	struct spdk_event_handler_opts eh_opts = {};
	/* [한국어] 정규화된 내부 옵션(기본값+사용자 옵션 병합). */
	struct spdk_fd_group *root;
	int rc;

	/* parameter checking */
	if (fgrp == NULL || efd < 0 || fn == NULL) {
		/* [한국어] 필수 인자 누락 거부. */
		return -EINVAL;
	}

	spdk_fd_group_get_default_event_handler_opts(&eh_opts, sizeof(eh_opts));
	/* [한국어] 기본값으로 시작. */
	if (opts) {
		event_handler_opts_copy(opts, &eh_opts);
		/* [한국어] 호출자 옵션이 있으면 ABI 호환적으로 덮어쓰기. */
	}

	/* check if there is already one function registered for this fd */
	TAILQ_FOREACH(ehdlr, &fgrp->event_handlers, next) {
		if (ehdlr->fd == efd) {
			/* [한국어] 같은 fd가 이미 등록 - 중복 등록 금지(EEXIST). */
			return -EEXIST;
		}
	}

	/* create a new event src */
	ehdlr = calloc(1, sizeof(*ehdlr));
	/* [한국어] 핸들러 노드를 0초기화 할당(일반 힙). */
	if (ehdlr == NULL) {
		return -errno;
	}

	ehdlr->fd = efd;
	/* [한국어] 모니터링 대상 fd 저장. */
	ehdlr->fn = fn;
	/* [한국어] 사용자 콜백 저장. */
	ehdlr->fn_arg = arg;
	/* [한국어] 콜백 컨텍스트 저장. */
	ehdlr->state = EVENT_HANDLER_STATE_WAITING;
	/* [한국어] 초기 상태 = WAITING(이벤트 대기 중, wait 루프 미진입). */
	ehdlr->events = eh_opts.events;
	/* [한국어] 옵션 적용. */
	ehdlr->fd_type = eh_opts.fd_type;
	/* [한국어] fd 타입 적용. */
	ehdlr->owner = fgrp;
	/* [한국어] 소유자 그룹 기록 - wrapper_fn 적용 여부를 owner 기준으로 결정. */
	snprintf(ehdlr->name, sizeof(ehdlr->name), "%s", name);
	/* [한국어] 이름 복사(truncate 안전). */

	root = fd_group_get_root(fgrp);
	/* [한국어] 실제 epfd를 보유한 루트 그룹 결정. */
	epevent.events = ehdlr->events;
	/* [한국어] epoll_ctl에 전달할 이벤트 마스크. */
	epevent.data.ptr = ehdlr;
	/* [한국어] 이벤트 발생 시 wait가 ehdlr를 복원할 수 있도록 포인터 부착. */
	rc = epoll_ctl(root->epfd, EPOLL_CTL_ADD, efd, &epevent);
	/* [한국어] 루트 epfd에 fd 등록. 자식 그룹의 fd라도 모니터링은 루트가 한다. */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add fd: %d to fd group(%p): %s\n",
			    efd, fgrp, strerror(errno));
		free(ehdlr);
		/* [한국어] epoll 등록 실패 시 핸들러 누수 방지. */
		return -errno;
	}

	TAILQ_INSERT_TAIL(&fgrp->event_handlers, ehdlr, next);
	/* [한국어] 그룹의 핸들러 리스트에 추가(나중에 destroy/wait에서 사용). */
	root->num_fds++;
	/* [한국어] 루트 카운트 증가(wait가 events 배열 크기로 사용). */

	return 0;
}

/*
 * [한국어]
 * spdk_fd_group_remove - fgrp에서 fd 등록 해제.
 *
 * 콜백이 디스패치 도중(RUNNING)이라면 free를 미루고 REMOVED 마크만 한다.
 * wait 루프가 끝에서 free 처리.
 */
void
spdk_fd_group_remove(struct spdk_fd_group *fgrp, int efd)
{
	struct event_handler *ehdlr;
	struct spdk_fd_group *root;
	int rc;

	if (fgrp == NULL || efd < 0) {
		SPDK_ERRLOG("Cannot remove fd: %d from fd group(%p)\n", efd, fgrp);
		assert(0);
		/* [한국어] 잘못된 인자는 디버그 빌드에서 즉시 중단 - 호출자 버그. */
		return;
	}


	TAILQ_FOREACH(ehdlr, &fgrp->event_handlers, next) {
		if (ehdlr->fd == efd) {
			/* [한국어] 매칭되는 핸들러 발견 시 루프 탈출. */
			break;
		}
	}

	if (ehdlr == NULL) {
		SPDK_ERRLOG("fd: %d doesn't exist in fd group(%p)\n", efd, fgrp);
		/* [한국어] 등록되지 않은 fd 제거 시도 - 호출자 버그 가능성. */
		return;
	}

	assert(ehdlr->state != EVENT_HANDLER_STATE_REMOVED);
	/* [한국어] 이미 REMOVED 상태면 두 번째 remove 호출 - 잘못된 사용. */
	root = fd_group_get_root(fgrp);

	rc = epoll_ctl(root->epfd, EPOLL_CTL_DEL, ehdlr->fd, NULL);
	/* [한국어] 루트 epfd에서 실제 fd 등록 해제. */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to remove fd: %d from fd group(%p): %s\n",
			    ehdlr->fd, fgrp, strerror(errno));
		assert(0);
		return;
	}

	assert(root->num_fds > 0);
	root->num_fds--;
	/* [한국어] 카운트 감소. */
	TAILQ_REMOVE(&fgrp->event_handlers, ehdlr, next);
	/* [한국어] 그룹 리스트에서 제거. */

	/* Delay ehdlr's free in case it is waiting for execution in fgrp wait loop */
	if (ehdlr->state == EVENT_HANDLER_STATE_RUNNING) {
		/* [한국어] 콜백 디스패치 도중 자기 자신을 remove 호출한 경우 (또는 다른 콜백이 호출):
		 * 즉시 free하면 wait 루프가 freed 메모리에 접근. 상태만 마킹하고 wait가 free. */
		ehdlr->state = EVENT_HANDLER_STATE_REMOVED;
	} else {
		free(ehdlr);
		/* [한국어] WAITING 상태 - 안전하게 즉시 free. */
	}
}

/*
 * [한국어]
 * spdk_fd_group_event_modify - 등록된 fd의 epoll 이벤트 마스크를 변경.
 *
 * @return: 0 성공 또는 epoll_ctl 결과, -EINVAL 잘못된 인자/미등록 fd.
 */
int
spdk_fd_group_event_modify(struct spdk_fd_group *fgrp,
			   int efd, int event_types)
{
	struct epoll_event epevent;
	struct event_handler *ehdlr;

	if (fgrp == NULL || efd < 0) {
		return -EINVAL;
	}

	TAILQ_FOREACH(ehdlr, &fgrp->event_handlers, next) {
		if (ehdlr->fd == efd) {
			break;
		}
	}

	if (ehdlr == NULL) {
		/* [한국어] 등록되지 않은 fd. */
		return -EINVAL;
	}

	assert(ehdlr->state != EVENT_HANDLER_STATE_REMOVED);
	/* [한국어] REMOVED 핸들러를 modify하면 안 됨 - 곧 free될 객체. */

	ehdlr->events = event_types;
	/* [한국어] 메모리 상의 마스크 갱신. */

	epevent.events = ehdlr->events;
	/* [한국어] epoll_ctl에 전달할 마스크. */
	epevent.data.ptr = ehdlr;
	/* [한국어] data.ptr 유지(MOD 후에도 wait가 ehdlr 복원 필요). */

	return epoll_ctl(fd_group_get_root(fgrp)->epfd, EPOLL_CTL_MOD, ehdlr->fd, &epevent);
	/* [한국어] 루트 epfd에 마스크 변경 요청. */
}

/*
 * [한국어]
 * spdk_fd_group_create - 새 fd_group 생성.
 *
 * @_egrp:  [out] 생성된 그룹 포인터 저장 위치.
 * @return: 0 성공, -EINVAL/-ENOMEM/-errno.
 *
 * epoll_create1(EPOLL_CLOEXEC)로 새 epoll 인스턴스 생성.
 */
int
spdk_fd_group_create(struct spdk_fd_group **_egrp)
{
	struct spdk_fd_group *fgrp;

	if (_egrp == NULL) {
		return -EINVAL;
	}

	fgrp = calloc(1, sizeof(*fgrp));
	/* [한국어] 그룹 헤더 0초기화 할당. */
	if (fgrp == NULL) {
		return -ENOMEM;
	}

	/* init the event source head */
	TAILQ_INIT(&fgrp->event_handlers);
	/* [한국어] 핸들러 리스트 빈 큐 초기화. */
	TAILQ_INIT(&fgrp->children);
	/* [한국어] 자식 그룹 리스트 빈 큐 초기화. */

	fgrp->num_fds = 0;
	/* [한국어] 카운트 명시적 0(이미 calloc로 0이지만 가독성). */
	fgrp->epfd = epoll_create1(EPOLL_CLOEXEC);
	/* [한국어] EPOLL_CLOEXEC: exec 시 자동 close - fork-exec 모델에서 fd 누수 방지. */
	if (fgrp->epfd < 0) {
		free(fgrp);
		/* [한국어] epoll 생성 실패 - 헤더 누수 방지. */
		return -errno;
	}

	*_egrp = fgrp;
	/* [한국어] 호출자에 결과 전달. */

	return 0;
}

/*
 * [한국어]
 * spdk_fd_group_destroy - fd_group 해제.
 *
 * 사전 조건: 등록된 fd가 없어야 하고 nest 관계도 분리되어 있어야 함.
 * 위반 시 디버그 빌드는 즉시 중단(assert).
 */
void
spdk_fd_group_destroy(struct spdk_fd_group *fgrp)
{
	if (fgrp == NULL || fgrp->num_fds > 0) {
		if (!fgrp) {
			SPDK_ERRLOG("fd_group doesn't exist.\n");
		} else {
			SPDK_ERRLOG("Cannot delete fd group(%p) as (%u) fds are still registered to it.\n",
				    fgrp, fgrp->num_fds);
		}
		assert(0);
		/* [한국어] 잘못된 destroy - 호출자 버그. */
		return;
	}

	/* Check if someone tried to delete the fd group before unnesting it */
	if (!TAILQ_EMPTY(&fgrp->event_handlers)) {
		SPDK_ERRLOG("Interrupt sources list not empty.\n");
		assert(0);
		/* [한국어] num_fds==0인데 리스트가 비지 않았다 - 카운트와 리스트 불일치 버그. */
		return;
	}

	assert(fgrp->parent == NULL);
	/* [한국어] nest된 채로 destroy 금지. */
	assert(TAILQ_EMPTY(&fgrp->children));
	/* [한국어] 자식이 nest되어 있으면 안 됨. */
	close(fgrp->epfd);
	/* [한국어] epoll 인스턴스 close - 커널 자원 회수. */
	free(fgrp);
	/* [한국어] 헤더 free. */

	return;
}

/*
 * [한국어]
 * spdk_fd_group_wait - 이 파일의 핵심: epoll_wait + 이벤트 디스패치 메인 루프.
 *
 * @fgrp:    wait 대상 그룹(루트여야 함; 자식이면 경고/에러).
 * @timeout: epoll_wait 타임아웃(밀리초). -1이면 무한, 0이면 즉시 반환.
 * @return:  처리한 이벤트 수, 0(타임아웃), -errno(에러).
 *
 * 동작 단계:
 *  1) 자식 그룹에 호출되었으면 무한 블록 위험 → 경고/에러 반환.
 *  2) epoll_wait로 등록된 fd들의 이벤트 수신.
 *  3) 첫 패스: 모든 활성 ehdlr를 RUNNING으로 마킹 (디스패치 중 다른 콜백이 같은 핸들러를
 *     remove하더라도 free 지연을 위해).
 *  4) 둘째 패스: 각 ehdlr에 대해
 *     - REMOVED면 free
 *     - EVENTFD면 read()로 카운터 리셋
 *     - g_event 설정 후 wrapper 또는 fn 직접 호출
 *     - 호출 후 REMOVED이면 free, 아니면 WAITING으로 복귀
 *
 * 호출처: lib/thread (interrupt mode) / lib/event reactor 인터럽트 루프.
 * 실행 컨텍스트: 단일 SPDK thread/reactor.
 */
int
spdk_fd_group_wait(struct spdk_fd_group *fgrp, int timeout)
{
	struct spdk_fd_group *owner;
	/* [한국어] 콜백을 등록한 owner 그룹(wrapper_fn 사용 여부 결정). */
	uint32_t totalfds = fgrp->num_fds;
	/* [한국어] events 배열 크기 결정용. */
	struct epoll_event events[totalfds];
	/* [한국어] VLA(가변 길이 배열)로 스택에 events 버퍼 할당. C99/GNU 확장.
	 * num_fds가 매우 크면 스택 오버플로 주의. */
	struct event_handler *ehdlr;
	uint64_t count;
	/* [한국어] eventfd read에 사용할 카운터 버퍼(eventfd는 8바이트 카운터 의미론). */
	int n;
	int nfds;
	int bytes_read;
	int read_errno;

	if (fgrp->parent != NULL) {
		/* [한국어] 자식 그룹은 자체 epfd가 비어있어 epoll_wait가 영구 블록될 위험. */
		if (timeout < 0) {
			SPDK_ERRLOG("Calling spdk_fd_group_wait on a group nested in another group without a timeout will block indefinitely.\n");
			assert(false);
			return -EINVAL;
		} else {
			SPDK_WARNLOG("Calling spdk_fd_group_wait on a group nested in another group will never find any events.\n");
			return 0;
		}
	}

	nfds = epoll_wait(fgrp->epfd, events, totalfds, timeout);
	/* [한국어] 핵심: 커널이 등록된 fd들을 모니터링, 이벤트 발생까지 또는 timeout까지 블록.
	 * epoll_wait는 NVMe completion이 아닌 일반 인터럽트성 fd(eventfd 등)에 사용. */
	if (nfds < 0) {
		if (errno != EINTR) {
			SPDK_ERRLOG("fd group(%p) epoll_wait failed: %s\n",
				    fgrp, strerror(errno));
		}
		/* [한국어] EINTR(시그널 인터럽트)은 일상적이라 로그 안 함. */

		return -errno;
	} else if (nfds == 0) {
		/* [한국어] 타임아웃 - 정상 케이스. */
		return 0;
	}

	for (n = 0; n < nfds; n++) {
		/* find the event_handler */
		ehdlr = events[n].data.ptr;
		/* [한국어] add 시 부착했던 ehdlr 포인터 복원. */

		if (ehdlr == NULL) {
			continue;
		}

		/* Tag ehdlr as running state in case that it is removed
		 * during this wait loop but before or when it get executed.
		 */
		assert(ehdlr->state == EVENT_HANDLER_STATE_WAITING);
		/* [한국어] wait 진입 시점에는 모든 핸들러가 WAITING이어야 함. */
		ehdlr->state = EVENT_HANDLER_STATE_RUNNING;
		/* [한국어] RUNNING으로 마킹 - 디스패치 중에 remove되면 REMOVED로 마킹만 되고 free는 미뤄짐. */
	}

	for (n = 0; n < nfds; n++) {
		/* find the event_handler */
		ehdlr = events[n].data.ptr;

		if (ehdlr == NULL || ehdlr->fn == NULL) {
			continue;
		}

		/* It is possible that the ehdlr was removed
		 * during this wait loop but before it get executed.
		 */
		if (ehdlr->state == EVENT_HANDLER_STATE_REMOVED) {
			/* [한국어] 첫 패스 이후 다른 콜백이 이 핸들러를 remove한 경우 - 즉시 free. */
			free(ehdlr);
			continue;
		}

		g_event = &events[n];
		/* [한국어] 콜백 내부에서 spdk_fd_group_get_epoll_event로 조회 가능하도록 thread-local 설정. */

		/* read fd to reset the internal eventfd object counter value to 0 */
		if (ehdlr->fd_type == SPDK_FD_TYPE_EVENTFD) {
			/* [한국어] eventfd는 read하지 않으면 카운터가 누적되어 다음 wait도 즉시 깨움. */
			bytes_read = read(ehdlr->fd, &count, sizeof(count));
			/* [한국어] 8바이트 카운터 읽고 0으로 리셋. */
			if (bytes_read < 0) {
				g_event = NULL;
				if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN) {
					/* [한국어] 일시적 - 그냥 다음 이벤트로. */
					continue;
				}
				read_errno = errno;
				/* TODO: Device is buggy. Handle this properly */
				SPDK_ERRLOG("Failed to read fd (%d) %s\n",
					    ehdlr->fd, strerror(errno));
				return -read_errno;
			} else if (bytes_read == 0) {
				SPDK_ERRLOG("Read nothing from fd (%d)\n", ehdlr->fd);
				g_event = NULL;
				return -EINVAL;
				/* [한국어] eventfd에서 0바이트 read는 이상 상황 - 에러. */
			}
		}

		/* call the interrupt response function */
		owner = ehdlr->owner;
		/* [한국어] 핸들러 등록 시 기록한 소유 그룹. */
		if (owner->wrapper_fn != NULL) {
			owner->wrapper_fn(owner->wrapper_arg, ehdlr->fn, ehdlr->fn_arg);
			/* [한국어] wrapper가 있으면 wrapper가 콜백 호출 컨텍스트를 통제(예: SPDK thread 진입). */
		} else {
			ehdlr->fn(ehdlr->fn_arg);
			/* [한국어] wrapper 없으면 콜백 직접 호출. */
		}
		g_event = NULL;
		/* [한국어] 콜백 종료 후 thread-local 클리어 - 다른 컨텍스트에서 잘못 접근 방지. */

		/* It is possible that the ehdlr was removed
		 * during this wait loop when it get executed.
		 */
		if (ehdlr->state == EVENT_HANDLER_STATE_REMOVED) {
			/* [한국어] 콜백이 자기 자신을 remove한 경우 - 이제 안전하게 free. */
			free(ehdlr);
		} else {
			ehdlr->state = EVENT_HANDLER_STATE_WAITING;
			/* [한국어] 정상 종료 - 다음 wait를 위해 WAITING 복귀. */
		}
	}

	return nfds;
}

/*
 * [한국어]
 * spdk_fd_group_set_wrapper - 콜백 호출을 감싸는 wrapper 함수 설정.
 *
 * @return: 0 성공, -EEXIST(이미 wrapper 있음 + 새 fn도 NULL이 아닌 경우),
 *          -EINVAL(자식 그룹이 있으면 nest와 충돌하므로 거부).
 *
 * 사용처: SPDK thread가 인터럽트 모드 콜백을 자기 thread 컨텍스트에서 실행하기 위해
 * thread 진입/탈출 처리를 wrapper로 끼워 넣음.
 */
int
spdk_fd_group_set_wrapper(struct spdk_fd_group *fgrp, spdk_fd_group_wrapper_fn fn, void *ctx)
{
	if (fgrp->wrapper_fn != NULL && fn != NULL) {
		/* [한국어] 이미 다른 wrapper가 있는데 또 다른 wrapper로 덮어쓰려는 경우 거부.
		 * fn==NULL은 wrapper 해제로 허용. */
		return -EEXIST;
	}

	if (!TAILQ_EMPTY(&fgrp->children)) {
		/* [한국어] 자식이 있으면 wrapper와 nest의 의미론이 충돌 - 거부. */
		return -EINVAL;
	}

	fgrp->wrapper_fn = fn;
	/* [한국어] wrapper 함수 설정(NULL이면 해제). */
	fgrp->wrapper_arg = ctx;
	/* [한국어] wrapper에 전달할 컨텍스트 설정. */

	return 0;
}

#else /* !__linux__ */
/* [한국어] 비-Linux 빌드: epoll API가 없으므로 모든 함수는 -ENOTSUP를 반환하는 stub.
 * SPDK는 비-Linux에서는 인터럽트 모드 자체가 사용되지 않는다. */

/*
 * [한국어]
 * 이하 모든 함수는 비-Linux 빌드 stub. 인터럽트 모드를 사용하지 않는 환경에서
 * 컴파일 오류 없이 의존 모듈을 빌드하기 위한 NO-OP 구현.
 */
int
spdk_fd_group_get_epoll_event(struct epoll_event *event)
{
	return -ENOTSUP;
}

int
spdk_fd_group_add(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn,
		  void *arg, const char *name)
{
	return -ENOTSUP;
}

int
spdk_fd_group_add_for_events(struct spdk_fd_group *fgrp, int efd, uint32_t events, spdk_fd_fn fn,
			     void *arg, const char *name)
{
	return -ENOTSUP;
}

int
spdk_fd_group_add_ext(struct spdk_fd_group *fgrp, int efd, spdk_fd_fn fn, void *arg,
		      const char *name, struct spdk_event_handler_opts *opts)
{
	return -ENOTSUP;
}

void
spdk_fd_group_get_default_event_handler_opts(struct spdk_event_handler_opts *opts,
		size_t opts_size)
{
	assert(false);
	/* [한국어] 비-Linux에서는 호출되어선 안 되는 경로 - 디버그 빌드에서 즉시 중단. */
}

void
spdk_fd_group_remove(struct spdk_fd_group *fgrp, int efd)
{
	/* [한국어] no-op stub. */
}

int
spdk_fd_group_event_modify(struct spdk_fd_group *fgrp,
			   int efd, int event_types)
{
	return -ENOTSUP;
}

int
spdk_fd_group_create(struct spdk_fd_group **fgrp)
{
	return -ENOTSUP;
}

void
spdk_fd_group_destroy(struct spdk_fd_group *fgrp)
{
	/* [한국어] no-op stub. */
}

int
spdk_fd_group_wait(struct spdk_fd_group *fgrp, int timeout)
{
	return -ENOTSUP;
}

int
spdk_fd_group_unnest(struct spdk_fd_group *parent, struct spdk_fd_group *child)
{
	return -ENOTSUP;
}

int
spdk_fd_group_nest(struct spdk_fd_group *parent, struct spdk_fd_group *child)
{
	return -ENOTSUP;
}

int
spdk_fd_group_set_wrapper(struct spdk_fd_group *fgrp, spdk_fd_group_wrapper_fn fn, void *ctx)
{
	return -ENOTSUP;
}

#endif /* __linux__ */
