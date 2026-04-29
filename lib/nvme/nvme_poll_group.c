/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2021 Mellanox Technologies LTD.
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] NVMe 다중 트랜스포트 폴링 그룹 (Poll Group) 구현 (nvme_poll_group.c)
 *
 * === 파일의 역할 ===
 * SPDK NVMe 드라이버에서 "여러 qpair를 단일 process_completions 호출로 한꺼번에
 * 폴링/관리"하는 최상위 추상 — `struct spdk_nvme_poll_group`의 구현 파일.
 * 이 파일이 제공하는 핵심 가치는 다음 4가지:
 *   1) **트랜스포트 이종 통합**: PCIe + RDMA + TCP + vfio-user 등 서로 다른 트랜스포트의
 *      qpair들을 하나의 그룹으로 묶고, 사용자에게는 단일 API로 노출. 내부적으로
 *      트랜스포트별 sub-group(`struct spdk_nvme_transport_poll_group` = 이하 tgroup)을
 *      유지하여 트랜스포트별 최적화된 폴링 루틴을 호출한다.
 *   2) **accel 오프로드 통합**: CRC32C/메모리 복사 등 데이터 변환을 SPDK accel 프레임워크
 *      (DSA/IDXD/AVX 등)로 위임하기 위한 함수 테이블(`spdk_nvme_accel_fn_table`)을
 *      poll group 단위로 보관. ABI 호환을 위해 `table_size` 기반 SET_FIELD 매크로 사용.
 *   3) **인터럽트 모드(epoll) 통합**: 폴링 모드가 아닌 interrupt 모드에서는 각 qpair가
 *      자신의 completion fd(eventfd 또는 RDMA comp_channel fd)를 가지며, 이들을 모두
 *      `spdk_fd_group`(epoll 래퍼)에 등록해 단일 epoll_wait로 깨어나도록 함.
 *      `spdk_nvme_poll_group_wait()`가 그 진입점.
 *   4) **disconnected qpair 관통 통지**: 트랜스포트가 비동기 link loss/reset을 감지하면
 *      해당 qpair를 tgroup의 `disconnected_qpairs` 리스트로 옮겨두고, 사용자가 매 폴링마다
 *      넘긴 `disconnected_qpair_cb`로 통지받아 재연결/제거 결정을 내릴 수 있다.
 *
 * **본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본**이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호스트 SPDK 애플리케이션의 일반적인 코어 레이아웃:
 *   1 reactor(=1 CPU 코어) → 1 spdk_thread → 1 spdk_nvme_poll_group → N qpair
 * 상위 사용자(예: bdev_nvme 모듈, 사용자 애플리케이션)는 모든 NVMe qpair를 자기 코어의
 * poll_group에 등록한 뒤, 메인 루프에서 주기적으로
 * `spdk_nvme_poll_group_process_completions(group, 0, disconnected_cb)`를 호출하면 끝이다.
 * 그러면 이 파일이:
 *   group → tgroups[PCIe, RDMA, TCP, ...] STAILQ 순회 →
 *     각 tgroup마다 transport vtable의 poll_group_process_completions 호출 →
 *       해당 트랜스포트가 자기만의 방식(예: PCIe는 모든 qpair의 phase bit 폴링,
 *       RDMA는 ibv_poll_cq, TCP는 sock_group_poll)으로 완료 수집 →
 *         qpair별 cb_fn 호출.
 *
 * 호출 체인 (제출 경로의 connect/disconnect는 이 파일 + nvme_transport.c가 협력):
 *   spdk_nvme_ctrlr_connect_io_qpair (nvme_ctrlr.c)
 *     → nvme_transport_ctrlr_connect_qpair (nvme_transport.c)
 *       → poll_group이 있으면 nvme_poll_group_connect_qpair (이 파일)
 *         → nvme_transport_poll_group_connect_qpair (nvme_transport.c, 트랜스포트 위임)
 *         → nvme_poll_group_add_qpair_fd (이 파일, interrupt 모드 한정)
 *
 * 호출 체인 (완료 경로):
 *   사용자 애플리케이션 메인 루프
 *     → spdk_nvme_poll_group_process_completions (이 파일)
 *       → 각 tgroup마다 nvme_transport_poll_group_process_completions (nvme_transport.c)
 *         → 트랜스포트별 vtable.poll_group_process_completions
 *           → qpair별 spdk_nvme_qpair_process_completions (nvme_qpair.c)
 *             → 트랜스포트별 vtable.qpair_process_completions
 *               → 실제 CQE 수확 + cb_fn 호출
 *
 * 실행 컨텍스트: poll_group은 **단일 spdk_thread에 고정(affinity)** 된다. 즉 poll_group을
 * 만든 스레드 외에서는 add/remove/process_completions 호출이 금지(또는 race를
 * 사용자 책임으로 둠)이며, 이 덕분에 내부에 락 없이 STAILQ 조작이 가능하다.
 * 단 SPDK 표준 패턴은 reactor 1코어=1 poll_group이므로 자연히 단일 스레드성이 유지된다.
 *
 * === 타 모듈과의 연결 ===
 *  - **nvme_transport.c** (`nvme_transport_poll_group_*` 9종) — 이 파일은 트랜스포트별
 *    sub-group의 생성/삭제/추가/제거/connect/disconnect/process/check_disconnected/stats
 *    9개 진입점을 호출만 하고, 실제 동작은 nvme_transport.c의 vtable 디스패치 → 트랜스포트
 *    드라이버 콜백(예: nvme_pcie_common.c의 `nvme_pcie_poll_group_*`)이 수행.
 *  - **nvme_qpair.c** — `nvme_qpair_get_state`, `spdk_nvme_qpair_process_completions`,
 *    `spdk_nvme_qpair_get_fd` 사용. qpair 상태 머신 진입 조건(DISCONNECTED 시에만 add 허용)
 *    검증과 interrupt 모드 fd 추출의 협력자.
 *  - **fd_group (include/spdk/fd_group.h)** — `spdk_fd_group_create/destroy/wait`,
 *    `SPDK_FD_GROUP_ADD_EXT/REMOVE`. interrupt 모드에서 모든 qpair completion fd +
 *    disconnect 알림 eventfd를 묶어 단일 epoll로 처리.
 *  - **eventfd (Linux)** — `eventfd(EFD_NONBLOCK | EFD_CLOEXEC)`. disconnected 발생 시
 *    `nvme_poll_group_write_disconnect_qpair_fd`에서 이 fd에 1을 write → epoll_wait가 깨어남.
 *  - **상위 사용자 (bdev_nvme, 애플리케이션)** — 거의 모든 SPDK NVMe 사용자는 이 poll_group
 *    API의 클라이언트. spdk_nvme_poll_group_create로 1개 만들고 모든 qpair를 add 후
 *    process_completions를 reactor 매 tick마다 호출.
 *
 * 데이터 흐름:
 *   생성: 사용자 → spdk_nvme_poll_group_create → group{ctx, accel_fn_table, fgrp, tgroups[]}
 *   등록: 사용자(qpair) → spdk_nvme_poll_group_add → 트랜스포트별 tgroup 매칭/생성 →
 *         tgroup->qpair 리스트 추가. interrupt 모드면 qpair fd를 fgrp에 등록.
 *   폴링: 사용자 → spdk_nvme_poll_group_process_completions → 모든 tgroup 순회 →
 *         transport vtable → qpair 완료 수확 → 사용자 cb_fn 실행.
 *
 * === 주요 함수/구조체 요약 ===
 *  핵심 자료구조 (정의는 nvme_internal.h:854~903 — 이 파일에서 사용만):
 *    - spdk_nvme_poll_group: ctx, accel_fn_table, tgroups STAILQ, fgrp(epoll),
 *      enable_interrupts 플래그, disconnect_qpair_fd(eventfd), interrupt 콜백.
 *    - spdk_nvme_transport_poll_group: 상위 group 포인터, transport vtable,
 *      connected/disconnected qpair STAILQ 두 개, num_connected_qpairs 캐시.
 *
 *  주요 함수 (사용자 노출 + 내부 헬퍼):
 *    [생성/소멸]
 *    - spdk_nvme_poll_group_create — group 할당 + accel_fn_table 검증/복사 + fgrp 생성
 *    - spdk_nvme_poll_group_destroy — 모든 tgroup 정리 + fgrp/eventfd close
 *
 *    [qpair 관리]
 *    - spdk_nvme_poll_group_add — qpair를 트랜스포트별 tgroup에 등록 (없으면 tgroup 생성)
 *    - spdk_nvme_poll_group_remove — qpair를 tgroup에서 제거
 *    - nvme_poll_group_connect_qpair — 트랜스포트 connect + interrupt 모드면 fd 등록
 *    - nvme_poll_group_disconnect_qpair — fd 제거 + 트랜스포트 disconnect
 *
 *    [폴링 루프]
 *    - spdk_nvme_poll_group_process_completions — ★ 메인 폴링 진입점. 모든 tgroup 순회.
 *      재귀 방지(in_process_completions), 음수 반환 처리(error_reason 누적).
 *    - spdk_nvme_poll_group_wait — interrupt 모드에서 epoll_wait 호출
 *    - spdk_nvme_poll_group_all_connected — 모든 qpair 상태 확인 (-EIO/-EAGAIN/0)
 *
 *    [interrupt 모드 보조]
 *    - spdk_nvme_poll_group_get_fd_group — 외부 epoll 통합용 fgrp getter
 *    - spdk_nvme_poll_group_set_interrupt_callback — disconnected 통지 콜백 등록
 *    - nvme_poll_group_write_disconnect_qpair_fd — eventfd write로 epoll wake
 *    - nvme_poll_group_read_disconnect_qpair_fd — eventfd 트리거 시 사용자 콜백 호출
 *    - nvme_poll_group_add/remove_disconnect_qpair_fd — eventfd 생성/등록
 *    - nvme_poll_group_add/remove_qpair_fd — qpair completion fd를 fgrp에 등록/제거
 *    - nvme_qpair_process_completion_wrapper — fgrp 콜백 (epoll 트리거 시 호출)
 *
 *    [통계/컨텍스트]
 *    - spdk_nvme_poll_group_get_ctx — 사용자 ctx getter
 *    - spdk_nvme_poll_group_get_stats — 모든 트랜스포트 stats 수집
 *    - spdk_nvme_poll_group_free_stats — stats 메모리 해제 (트랜스포트별 free 위임)
 */

#include "nvme_internal.h"
                                  /* [한국어] NVMe 드라이버 내부 정의 — spdk_nvme_poll_group/transport_poll_group
                                   *  구조체 정의(854~903), nvme_transport_poll_group_* 프로토타입(2561~2577),
                                   *  nvme_qpair_get_state, NVME_QPAIR_DISCONNECTED enum, NVME_QPAIR_ERRLOG 매크로 등 */

/*
 * [한국어]
 * spdk_nvme_poll_group_create - 새 NVMe poll group 생성 (사용자 노출 API의 진입점)
 *
 * @ctx:   사용자 컨텍스트 포인터 — 나중에 spdk_nvme_poll_group_get_ctx로 회수.
 *         일반적으로 이 poll_group을 소유한 상위 객체(예: bdev_nvme channel) 포인터.
 * @table: accel(가속 오프로드) 함수 테이블. NULL이면 accel 미사용. 사용자가 SPDK accel
 *         프레임워크를 통합하면 finish_sequence/append_crc32c/append_copy 등 콜백을
 *         채워 전달. 라이브러리는 ABI 호환성을 위해 table->table_size 만큼만 복사.
 * @return 새 poll_group 포인터 또는 NULL(메모리 부족 / accel_fn_table 검증 실패).
 *
 * 동작 단계:
 *   [1] calloc로 0 초기화된 group 할당.
 *   [2] accel_fn_table.table_size 기본값을 라이브러리 컴파일 시 크기로 세팅.
 *       사용자가 NULL 아닌 table을 줬다면 SET_FIELD 매크로로 필드별 ABI 안전 복사.
 *       (각 필드 offset+size <= table_size 일 때만 복사 → 사용자가 구버전 헤더로
 *       빌드했어도 새 필드는 라이브러리 기본값 유지)
 *   [3] accel 콜백 일관성 2단계 검증:
 *       (a) finish/reverse/abort 3종은 "전부 제공" 또는 "전부 NULL" 둘 중 하나.
 *           XOR 형태로 (AND != OR) 비교 — 일부만 채우면 실패.
 *       (b) append_* 콜백을 제공했다면 finish_sequence는 반드시 있어야 함
 *           (sequence를 만들었으면 닫을 수도 있어야 하니까).
 *   [4] fd_group 생성 시도. interrupt 모드일 때 모든 qpair fd + disconnect eventfd를
 *       묶을 epoll 컨테이너. Linux 외 플랫폼은 미지원이므로 실패해도 무시.
 *   [5] disconnect_qpair_fd 초기값 -1, ctx 저장, tgroups 빈 STAILQ 초기화.
 *
 * 호출 컨텍스트: 사용자 코드(예: spdk_thread context)에서 1회 호출. 보통 reactor 1코어당 1번.
 *
 * 호출 체인:
 *   사용자 애플리케이션 → spdk_nvme_poll_group_create
 *                      → calloc + spdk_fd_group_create (interrupt 모드 한정)
 */
struct spdk_nvme_poll_group *
spdk_nvme_poll_group_create(void *ctx, struct spdk_nvme_accel_fn_table *table)
{
	struct spdk_nvme_poll_group *group;
                                  /* [한국어] 새로 만들 poll_group 객체 — 함수 끝에서 호출자에게 반환 */
	int rc;
                                  /* [한국어] spdk_fd_group_create 반환값 임시 보관 */

	group = calloc(1, sizeof(*group));
                                  /* [한국어] 0으로 초기화된 메모리 할당 — 모든 STAILQ/플래그가 0/NULL로 시작.
                                   *  calloc 사용 이유: bool 필드들과 포인터들이 명시적 초기화 없이도 false/NULL 보장. */
	if (group == NULL) {
                                  /* [한국어] OOM — 호출자에게 NULL 반환하여 실패 시그널링 */
		return NULL;
	}

	group->accel_fn_table.table_size = sizeof(struct spdk_nvme_accel_fn_table);
                                  /* [한국어] table_size 기본값을 라이브러리가 인식하는 크기로 세팅.
                                   *  사용자가 table=NULL로 호출하면 이 값만 남고 모든 콜백은 NULL → accel 비활성. */
	if (table && table->table_size != 0) {
                                  /* [한국어] 사용자가 accel 테이블을 제공했고 의미 있는 크기라면 필드 복사 진행 */
		group->accel_fn_table.table_size = table->table_size;
                                  /* [한국어] 사용자가 알린 크기를 그대로 보존 → 이후 SET_FIELD 매크로의 한계로 사용 */
#define SET_FIELD(field) \
	if (offsetof(struct spdk_nvme_accel_fn_table, field) + sizeof(table->field) <= table->table_size) { \
		group->accel_fn_table.field = table->field; \
	} \
                                  /* [한국어] ABI 호환 필드 복사 매크로:
                                   *   - 사용자 헤더와 라이브러리 헤더 버전이 다를 수 있음.
                                   *   - 각 필드의 offset+sizeof가 table_size 이내여야만 복사 → 구버전 사용자가
                                   *     아직 모르는 필드는 자동으로 라이브러리 기본값(NULL) 유지.
                                   *   - 새 필드 추가 시 SET_FIELD 라인을 추가하고 SPDK_STATIC_ASSERT 크기 갱신. */

		SET_FIELD(append_crc32c);
                                  /* [한국어] CRC32C append 콜백 — 데이터 무결성 체크섬을 SPDK accel(DSA 등)에 위임 */
		SET_FIELD(append_copy);
                                  /* [한국어] 메모리 복사 append 콜백 — DMA off-CPU 복사로 대역 절약 */
		SET_FIELD(finish_sequence);
                                  /* [한국어] sequence 종료 — append된 작업들을 한꺼번에 실행하고 cb_fn으로 결과 통지 */
		SET_FIELD(reverse_sequence);
                                  /* [한국어] sequence 반전 — read 경로에서 NVMe → host 방향으로 변환 순서 뒤집기 */
		SET_FIELD(abort_sequence);
                                  /* [한국어] sequence 중단 — 에러 발생 시 보류 작업 모두 취소 */
		/* Do not remove this statement, you should always update this statement when you adding a new field,
		 * and do not forget to add the SET_FIELD statement for your added field. */
		SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_accel_fn_table) == 56, "Incorrect size");
                                  /* [한국어] 컴파일 타임 가드 — 누군가 구조체에 필드 추가하면 크기가 변하므로
                                   *  이 assert가 깨짐. 깨진 사람이 위 SET_FIELD 라인도 추가한 뒤 56을 새 크기로 갱신해야 함.
                                   *  (table_size: 8 + reserved8: 8 + 콜백 5종 × 8B = 56B) */

#undef SET_FIELD
                                  /* [한국어] 매크로 스코프 종료 — 다른 파일/함수와 충돌 방지 위해 즉시 #undef */
	}

	/* Make sure either all or none of the sequence manipulation callbacks are implemented */
	if ((group->accel_fn_table.finish_sequence && group->accel_fn_table.reverse_sequence &&
	     group->accel_fn_table.abort_sequence) !=
	    (group->accel_fn_table.finish_sequence || group->accel_fn_table.reverse_sequence ||
	     group->accel_fn_table.abort_sequence)) {
                                  /* [한국어] sequence 3종(finish/reverse/abort)이 부분적으로만 채워져 있는지 검증.
                                   *   - 좌변(AND): 셋 모두 non-NULL이면 true
                                   *   - 우변(OR):  셋 중 하나라도 non-NULL이면 true
                                   *   - 둘이 다르다 = "AND=false ∧ OR=true" → 일부만 제공된 상태(불완전)
                                   *   - 둘이 같다  = "전부 NULL"(우변·좌변 모두 false) 또는 "전부 제공"(둘 다 true) → 정상
                                   *  부분 제공 시 sequence 수명주기 관리가 깨지므로 즉시 거부. */
		SPDK_ERRLOG("Invalid accel_fn_table configuration: either all or none of the "
			    "sequence callbacks must be provided\n");
                                  /* [한국어] 사용자에게 잘못된 설정임을 로그로 통지 (디버깅 도움) */
		free(group);
                                  /* [한국어] 누수 방지를 위해 calloc한 group 해제 */
		return NULL;
                                  /* [한국어] 검증 실패 → 생성 실패 시그널 */
	}

	/* Make sure that sequence callbacks are implemented if append* callbacks are provided */
	if ((group->accel_fn_table.append_crc32c || group->accel_fn_table.append_copy) &&
	    !group->accel_fn_table.finish_sequence) {
                                  /* [한국어] append_*는 sequence 안에 작업을 쌓는 동작이므로, sequence를 끝맺을
                                   *  finish_sequence가 없으면 영원히 실행되지 않음 → 의미 없는 구성. */
		SPDK_ERRLOG("Invalid accel_fn_table configuration: append_crc32c and/or append_copy require sequence "
			    "callbacks to be provided\n");
		free(group);
                                  /* [한국어] 잘못된 구성 — 누수 방지 */
		return NULL;
	}

	/* If interrupt is enabled, this fd_group will be used to manage events triggerd on file
	 * descriptors of all the qpairs in this poll group */
	rc = spdk_fd_group_create(&group->fgrp);
                                  /* [한국어] interrupt 모드 사용을 위한 epoll 컨테이너 생성.
                                   *  spdk_fd_group은 epoll_create + 각 fd별 콜백 매핑 자료구조.
                                   *  반환된 fgrp 포인터는 group->fgrp에 저장.
                                   *  Linux는 항상 성공 가정, 그 외 플랫폼은 -ENOTSUP 가능. */
	if (rc) {
		/* Ignore this for non-Linux platforms, as fd_groups aren't supported there. */
#if defined(__linux__)
                                  /* [한국어] Linux에서는 fd_group이 반드시 동작해야 함 — 실패 시 fail-fast */
		SPDK_ERRLOG("Cannot create fd group for the nvme poll group\n");
		free(group);
                                  /* [한국어] 할당 누수 방지 */
		return NULL;
#endif
                                  /* [한국어] 비-Linux 환경(FreeBSD 등): fd_group 미지원이 정상 — fgrp는 NULL로 둔 채 진행.
                                   *  이 경우 interrupt 모드 사용 시 enable_interrupts 검증에서 추가 거부됨. */
	}

	group->disconnect_qpair_fd = -1;
                                  /* [한국어] 아직 eventfd를 만들지 않은 상태 — sentinel.
                                   *  실제 fd는 첫 qpair add 시 enable_interrupts=true면 nvme_poll_group_add_disconnect_qpair_fd에서 생성. */
	group->ctx = ctx;
                                  /* [한국어] 사용자 컨텍스트 보존 — 나중에 get_ctx로 회수 */
	STAILQ_INIT(&group->tgroups);
                                  /* [한국어] 트랜스포트별 sub-group 리스트 빈 상태로 초기화.
                                   *  첫 qpair add 시 해당 트랜스포트의 tgroup이 lazy-create. */

	return group;
                                  /* [한국어] 사용자에게 완성된 poll_group 핸들 반환 */
}

/*
 * [한국어]
 * spdk_nvme_poll_group_get_fd_group - poll_group의 epoll 컨테이너(fd_group) getter
 *
 * @group: 대상 poll_group
 * @return group->fgrp (interrupt 모드용 epoll 래퍼) 또는 비-Linux 환경에서 NULL.
 *
 * 사용 시나리오:
 *   - 외부 reactor가 자기 epoll에 이 fd_group을 nest로 끼워 넣을 때
 *     (spdk_fd_group_nest로 계층 구성).
 *   - 사용자가 epoll fd를 직접 다른 시스템(예: io_uring 통합 wrapper)에 등록할 때.
 *
 * 호출 컨텍스트: poll_group 소유 스레드.
 */
struct spdk_fd_group *
spdk_nvme_poll_group_get_fd_group(struct spdk_nvme_poll_group *group)
{
	return group->fgrp;
                                  /* [한국어] 단순 필드 반환 — 별도 검증/락 없음. NULL일 수 있음(비-Linux) */
}

/*
 * [한국어]
 * spdk_nvme_poll_group_set_interrupt_callback - interrupt 모드 disconnected 통지 콜백 등록
 *
 * @group:  대상 poll_group
 * @cb_fn:  disconnected 이벤트 발생 시 호출될 사용자 콜백 (NULL이면 등록 해제)
 * @cb_ctx: cb_fn의 첫 인자
 * @return 0 성공, -EEXIST(이미 다른 cb가 등록되어 있는데 새 cb_fn이 NULL이 아닐 때).
 *
 * 동작:
 *   trasnport가 비동기로 qpair 끊김을 감지하면 nvme_poll_group_write_disconnect_qpair_fd로
 *   eventfd에 1을 write → epoll_wait가 깨어나면서 nvme_poll_group_read_disconnect_qpair_fd
 *   콜백이 자동 실행되고, 그 안에서 사용자 cb_fn을 호출.
 *
 *   사용자 cb_fn은 일반적으로 spdk_nvme_poll_group_process_completions를 즉시 호출해
 *   disconnected 이벤트를 자기 disconnected_qpair_cb로 받아 처리한다.
 *
 * 호출 체인:
 *   사용자 → spdk_nvme_poll_group_set_interrupt_callback (이 함수)
 *   ... 이후 비동기 발생 ...
 *   transport → nvme_poll_group_write_disconnect_qpair_fd → eventfd write
 *   epoll_wait 트리거 → nvme_poll_group_read_disconnect_qpair_fd → cb_fn 실행
 */
int
spdk_nvme_poll_group_set_interrupt_callback(struct spdk_nvme_poll_group *group,
		spdk_nvme_poll_group_interrupt_cb cb_fn, void *cb_ctx)
{
	if (group->interrupt.cb_fn != NULL && cb_fn != NULL) {
                                  /* [한국어] 이미 누군가 cb를 등록했고, 새로 들어온 것도 NULL이 아니면 충돌 → 거부.
                                   *  (등록 해제 의도라면 cb_fn=NULL을 명시적으로 줘야 함) */
		return -EEXIST;
	}

	group->interrupt.cb_fn = cb_fn;
                                  /* [한국어] 사용자 콜백 저장 — NULL이면 등록 해제 효과 */
	group->interrupt.cb_ctx = cb_ctx;
                                  /* [한국어] 콜백 첫 인자 저장 — 일반적으로 사용자 정의 컨텍스트 */

	return 0;
                                  /* [한국어] 성공 */
}

#ifdef __linux__
                                  /* [한국어] 이하 eventfd / epoll 의존 코드는 Linux 전용.
                                   *  비-Linux 빌드에서는 아래 #else 블록의 stub만 컴파일. */

/*
 * [한국어]
 * nvme_poll_group_read_disconnect_qpair_fd - disconnect 알림 eventfd가 트리거되면 호출되는 콜백
 *
 * @arg: SPDK_FD_GROUP_ADD_EXT 등록 시 넘긴 group 포인터 (void*로 형변환됨)
 * @return 0 (epoll 콜백 규약 — 처리한 이벤트 수)
 *
 * 동작:
 *   spdk_fd_group_wait → epoll_wait 반환 → SPDK_FD_TYPE_EVENTFD 자동 read 처리 →
 *   이 wrapper 호출. eventfd의 카운터는 fd_group이 자동 read하므로 여기서는 신경 안 써도 됨.
 *
 *   실제 disconnected 처리는 사용자 cb_fn이 spdk_nvme_poll_group_process_completions를
 *   재호출해서 disconnected_qpair_cb로 통지받게 하는 패턴.
 *
 * 실행 컨텍스트: poll_group 소유 스레드 (spdk_fd_group_wait 또는 spdk_nvme_poll_group_wait 내부).
 */
static int
nvme_poll_group_read_disconnect_qpair_fd(void *arg)
{
	struct spdk_nvme_poll_group *group = arg;
                                  /* [한국어] void* → 실제 타입 복원. ADD_EXT 인자 그대로 전달됨. */

	if (group->interrupt.cb_fn != NULL) {
                                  /* [한국어] 사용자가 set_interrupt_callback으로 cb를 등록했을 때만 호출 */
		group->interrupt.cb_fn(group, group->interrupt.cb_ctx);
                                  /* [한국어] 사용자 콜백 — 일반적으로 process_completions 재호출 또는 wakeup notify */
	}

	return 0;
                                  /* [한국어] fd_group/epoll에 "이번 이벤트 1개 처리 완료" 신호 */
}

/*
 * [한국어]
 * nvme_poll_group_write_disconnect_qpair_fd - 트랜스포트가 disconnect 발생을 알리기 위해 eventfd write
 *
 * @group: 대상 poll_group
 *
 * 호출자 (트랜스포트 콜백):
 *   - nvme_transport.c::nvme_transport_disconnect_qpair_done — 트랜스포트가 비동기로 끊김 감지 시
 *   - PCIe SIGBUS 핸들러 후속 정리 등
 *
 * 동작:
 *   group이 interrupt 모드가 아니면 즉시 return (eventfd 자체가 없음).
 *   interrupt 모드면 disconnect_qpair_fd에 8바이트 1을 write — eventfd 카운터 +=1 →
 *   이 fd가 들어가 있는 epoll fd가 readable 상태가 되어 epoll_wait가 깨어남.
 *   다음 spdk_nvme_poll_group_wait()/process_completions() 호출에서 disconnected_qpair_cb가 트리거됨.
 *
 * 비-Linux: 아래 #else 블록의 noop stub 사용.
 */
void
nvme_poll_group_write_disconnect_qpair_fd(struct spdk_nvme_poll_group *group)
{
	uint64_t notify = 1;
                                  /* [한국어] eventfd write의 페이로드 — 8바이트 정수.
                                   *  semaphore 모드가 아니므로 카운터에 누적되며, read하면 카운터 0으로 리셋됨.
                                   *  정확한 값은 무관하나 1로 관례적 사용. */
	int rc;

	if (!group->enable_interrupts) {
                                  /* [한국어] 폴링 모드면 eventfd 자체가 만들어지지 않았으므로 write 하면 안 됨.
                                   *  early return으로 보호. */
		return;
	}

	/* Write to the disconnect qpair fd. This will generate event on the epoll fd of poll
	 * group. We then check for disconnected qpairs either in spdk_nvme_poll_group_wait() or
	 * in transport's poll_group_process_completions() callback.
	 */
	rc = write(group->disconnect_qpair_fd, &notify, sizeof(notify));
                                  /* [한국어] eventfd에 8바이트 write — epoll 깨우기.
                                   *  EFD_NONBLOCK으로 만들었지만 실패는 거의 없음(64-bit 카운터 overflow도 사실상 불가).
                                   *  실패 시 다음 폴링 사이클에서 어차피 일반 process_completions가 disconnected를 발견함. */
	if (rc < 0) {
                                  /* [한국어] write 실패 — 매우 드문 경우(일반적으로 disconnect_qpair_fd가 이미 닫혔을 때) */
		SPDK_ERRLOG("failed to write the disconnect qpair fd: %s.\n", strerror(errno));
                                  /* [한국어] 디버깅용 로그만 남기고 더 이상 복구 시도하지 않음 */
	}
}

/*
 * [한국어]
 * nvme_poll_group_add_disconnect_qpair_fd - disconnect 알림용 eventfd 생성 + fd_group에 등록
 *
 * @group: 대상 poll_group
 * @return 0 성공, 음수(eventfd 생성 실패 또는 SPDK_FD_GROUP_ADD_EXT 실패).
 *
 * 호출 컨텍스트: spdk_nvme_poll_group_add 내부에서 첫 qpair 추가 시 (interrupt 모드인 경우만).
 *               poll_group 수명 동안 1회만 호출 — assert로 중복 방지.
 *
 * 동작:
 *   eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC) — 초기 카운터 0, non-blocking, exec 시 자동 close.
 *   group->disconnect_qpair_fd에 저장 + fd_group(epoll)에 read 콜백
 *   nvme_poll_group_read_disconnect_qpair_fd 등록.
 */
static int
nvme_poll_group_add_disconnect_qpair_fd(struct spdk_nvme_poll_group *group)
{
	struct spdk_event_handler_opts opts = {};
                                  /* [한국어] fd_group 콜백 등록 옵션 — 0 초기화 후 fd_type만 채움 */
	int fd;

	fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
                                  /* [한국어] Linux 전용 eventfd 생성:
                                   *   - 초기값 0 (아직 알림 없음)
                                   *   - EFD_NONBLOCK: 카운터 0일 때 read 호출 시 EAGAIN 반환 (블로킹 X)
                                   *   - EFD_CLOEXEC: fork+exec 시 자식 프로세스에 fd 누수 방지
                                   *  반환 fd는 read/write/epoll-wait 가능한 일반 fd처럼 동작. */
	if (fd < 0) {
                                  /* [한국어] eventfd() 실패 — fd 한도 초과 등 매우 드문 케이스 */
		return fd;
	}

	assert(group->disconnect_qpair_fd == -1);
                                  /* [한국어] 중복 호출 방지 — 이 함수는 poll_group 수명 동안 단 1회만 호출돼야 함.
                                   *  -1 sentinel이 깨졌다는 것은 호출자(spdk_nvme_poll_group_add) 로직 버그. */
	group->disconnect_qpair_fd = fd;
                                  /* [한국어] 생성된 fd를 group에 저장 — 이후 write 호출 + destroy 시 close에 사용 */

	spdk_fd_group_get_default_event_handler_opts(&opts, sizeof(opts));
                                  /* [한국어] opts 구조체에 라이브러리 기본값 채우기 (ABI 호환을 위한 sizeof 전달).
                                   *  이후 fd_type만 덮어씀. */
	opts.fd_type = SPDK_FD_TYPE_EVENTFD;
                                  /* [한국어] fd_group에게 "이건 eventfd임" 알려서, epoll 트리거 후 자동으로 read해서
                                   *  카운터 리셋해주도록 — 사용자 콜백은 read 신경 안 써도 됨. */

	return SPDK_FD_GROUP_ADD_EXT(group->fgrp, fd, nvme_poll_group_read_disconnect_qpair_fd,
				     group, &opts);
                                  /* [한국어] fd를 group->fgrp(epoll)에 등록 — 이후 epoll_wait가 트리거되면
                                   *  nvme_poll_group_read_disconnect_qpair_fd(group)이 자동 호출됨.
                                   *  매크로 SPDK_FD_GROUP_ADD_EXT는 위치 정보(file/line) 자동 첨부 + opts 전달. */
}

#else
                                  /* [한국어] 비-Linux 빌드: eventfd가 없으므로 stub 제공 */

void
nvme_poll_group_write_disconnect_qpair_fd(struct spdk_nvme_poll_group *group)
{
                                  /* [한국어] noop — interrupt 모드 자체가 비-Linux에서는 미지원이므로 무동작이 정상 */
}

static int
nvme_poll_group_add_disconnect_qpair_fd(struct spdk_nvme_poll_group *group)
{
	return -ENOTSUP;
                                  /* [한국어] interrupt 모드 미지원 명시 — 호출자(add)가 enable_interrupts=true 시 실패 처리 */
}

#endif

/*
 * [한국어]
 * spdk_nvme_poll_group_add - qpair를 poll_group에 등록 (사용자 노출 API)
 *
 * @group: 등록 대상 poll_group
 * @qpair: 등록할 NVMe qpair (반드시 DISCONNECTED 상태여야 함)
 * @return 0 성공, 음수 errno:
 *           -EINVAL: qpair가 DISCONNECTED 아님 (이미 연결됐거나 잘못된 상태)
 *           -EINVAL: 그룹의 interrupt 모드와 qpair 컨트롤러의 enable_interrupts 불일치
 *           -ENOMEM: 새 트랜스포트 sub-group 생성 메모리 부족
 *           -ENODEV: qpair의 트랜스포트가 등록되지 않은 미지원 트랜스포트
 *           기타 음수: 트랜스포트별 add 콜백의 에러
 *
 * 동작 단계:
 *   [1] qpair 상태 검증 — DISCONNECTED만 등록 가능 (connect는 add 후 별도로 호출).
 *   [2] enable_interrupts 정책 검증 — group은 첫 qpair의 컨트롤러 옵션으로 모드 고정.
 *       이후 등록되는 qpair들은 모두 같은 모드여야 함 (interrupt 모드 혼용 금지).
 *       첫 qpair가 interrupt 모드면 disconnect_qpair_fd 생성.
 *   [3] qpair 트랜스포트와 일치하는 tgroup 찾기 (STAILQ 선형 검색).
 *   [4] 없으면 lazy-create — 트랜스포트 레지스트리(g_spdk_nvme_transports TAILQ) 순회로
 *       해당 트랜스포트가 시스템에 로드되어 있는지 확인 후 nvme_transport_poll_group_create.
 *       (이는 dlopen으로 트랜스포트가 추가된 경우도 지원하기 위함)
 *   [5] tgroup이 있으면(생성됐든 존재했든) nvme_transport_poll_group_add로 위임.
 *
 * 호출 체인:
 *   사용자 (예: bdev_nvme_create_qpair) → spdk_nvme_poll_group_add
 *     → nvme_poll_group_add_disconnect_qpair_fd (interrupt 모드 첫 qpair 시)
 *     → nvme_transport_poll_group_create (해당 트랜스포트 첫 qpair 시)
 *     → nvme_transport_poll_group_add → 트랜스포트 vtable.poll_group_add
 */
int
spdk_nvme_poll_group_add(struct spdk_nvme_poll_group *group, struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_transport_poll_group *tgroup;
                                  /* [한국어] qpair가 들어갈 트랜스포트별 sub-group — 검색하거나 새로 만듦 */
	const struct spdk_nvme_transport *transport;
                                  /* [한국어] tgroup lazy-create 시 트랜스포트 레지스트리 순회용 임시 포인터 */
	int rc;

	if (nvme_qpair_get_state(qpair) != NVME_QPAIR_DISCONNECTED) {
                                  /* [한국어] poll_group 등록은 connect 이전에만 허용.
                                   *  이미 connect된 qpair를 add하면 connect 시점의 트랜스포트 fd 등록 등이 어긋남. */
		return -EINVAL;
	}

	if (!group->enable_interrupts_is_valid) {
                                  /* [한국어] 이 group에 처음 추가되는 qpair → interrupt 모드 결정 */
		group->enable_interrupts_is_valid = true;
                                  /* [한국어] 이후로는 모드 고정 — 다음 qpair부터는 일치 검증 */
		group->enable_interrupts = qpair->ctrlr->opts.enable_interrupts;
                                  /* [한국어] qpair 컨트롤러의 옵션을 group의 모드로 채택.
                                   *  컨트롤러 단위 옵션이므로 사용자가 spdk_nvme_ctrlr_opts.enable_interrupts로 설정. */
		if (group->enable_interrupts) {
                                  /* [한국어] interrupt 모드 첫 qpair → disconnect 알림용 eventfd 준비 */
			rc = nvme_poll_group_add_disconnect_qpair_fd(group);
			if (rc != 0) {
                                  /* [한국어] eventfd 생성 또는 fd_group 등록 실패 — 사용자에게 그대로 전파 */
				return rc;
			}
		}
	} else if (qpair->ctrlr->opts.enable_interrupts != group->enable_interrupts) {
                                  /* [한국어] 이미 모드가 정해진 group인데, 새 qpair의 컨트롤러 옵션이 다르면 불허.
                                   *  interrupt와 polling을 한 group 안에 섞으면 폴링 루프 의미가 무너짐. */
		NVME_QPAIR_ERRLOG(qpair, "Queue pair %s interrupts cannot be added to poll group\n",
				  qpair->ctrlr->opts.enable_interrupts ? "without" : "with");
                                  /* [한국어] 사용자에게 어떤 방향의 불일치인지 명시 ("with"는 새 qpair가 interrupt지만 group은 polling) */
		return -EINVAL;
	}

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
                                  /* [한국어] 이미 같은 트랜스포트의 sub-group이 있는지 검색.
                                   *  qpair의 transport 포인터(=vtable 주소)와 일치하면 그곳에 추가. */
		if (tgroup->transport == qpair->transport) {
			break;
		}
	}

	/* See if a new transport has been added (dlopen style) and we need to update the poll group */
	if (!tgroup) {
                                  /* [한국어] 위 STAILQ에서 못 찾았다면 첫 등록 — 트랜스포트 레지스트리에 있는지 확인하고 lazy-create.
                                   *  dlopen된 트랜스포트도 g_spdk_nvme_transports에 등록되므로 자동 발견 가능. */
		transport = nvme_get_first_transport();
                                  /* [한국어] 트랜스포트 레지스트리(TAILQ) 첫 항목 — nvme_transport.c::g_spdk_nvme_transports */
		while (transport != NULL) {
                                  /* [한국어] 모든 등록된 트랜스포트를 순회하며 qpair의 transport와 일치 여부 확인 */
			if (transport == qpair->transport) {
                                  /* [한국어] 일치 — 이 트랜스포트의 sub-group을 즉시 생성 */
				tgroup = nvme_transport_poll_group_create(transport);
                                  /* [한국어] 트랜스포트 vtable의 poll_group_create 호출 → 트랜스포트별 폴링 컨텍스트 할당
                                   *  (예: PCIe는 단순 STAILQ 컨테이너, RDMA는 ibv_comp_channel 등) */
				if (tgroup == NULL) {
                                  /* [한국어] 트랜스포트별 메모리 부족 등 */
					return -ENOMEM;
				}
				tgroup->group = group;
                                  /* [한국어] sub-group이 상위 group을 역참조할 수 있도록 back pointer 설정 */
				STAILQ_INSERT_TAIL(&group->tgroups, tgroup, link);
                                  /* [한국어] group의 tgroups 리스트 끝에 추가 — 이후 process_completions가 순회 */
				break;
			}
			transport = nvme_get_next_transport(transport);
                                  /* [한국어] 다음 트랜스포트로 전진 — TAILQ_NEXT 래퍼 */
		}
	}

	return tgroup ? nvme_transport_poll_group_add(tgroup, qpair) : -ENODEV;
                                  /* [한국어] tgroup 확보 시 트랜스포트별 add 콜백으로 위임 (qpair를 connected_qpairs에 잠시 넣지 않고
                                   *  보통 disconnected_qpairs로 옮긴 후 connect 시 connected로 이동).
                                   *  레지스트리에도 없는 트랜스포트면 -ENODEV. */
}

/*
 * [한국어]
 * spdk_nvme_poll_group_remove - qpair를 poll_group에서 제거 (사용자 노출 API)
 *
 * @group: 대상 poll_group
 * @qpair: 제거할 qpair (반드시 DISCONNECTED 상태여야 함 — disconnect 후 remove 순서)
 * @return 0 성공, 음수:
 *           -EINVAL: qpair가 DISCONNECTED 아님 (먼저 disconnect 필요)
 *           -ENODEV: 해당 트랜스포트 sub-group을 못 찾음 (이미 빠졌거나 잘못된 인자)
 *           기타: 트랜스포트별 remove 콜백의 에러
 *
 * 동작:
 *   상태 검증 → 트랜스포트별 tgroup 검색 → nvme_transport_poll_group_remove로 위임.
 *
 * 주의: tgroup 자체는 빈 상태가 되어도 제거하지 않음 — destroy 시 일괄 정리. 같은 트랜스포트의
 *       qpair가 다시 추가되면 기존 tgroup 재사용 가능.
 */
int
spdk_nvme_poll_group_remove(struct spdk_nvme_poll_group *group, struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_transport_poll_group *tgroup;

	if (nvme_qpair_get_state(qpair) != NVME_QPAIR_DISCONNECTED) {
                                  /* [한국어] DISCONNECTED 상태에서만 제거 허용 — 활성 I/O가 있는 qpair를 빼면 누수/UAF */
		return -EINVAL;
	}

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
                                  /* [한국어] 트랜스포트별 sub-group 순회하여 qpair의 transport와 일치하는 것 찾기 */
		if (tgroup->transport == qpair->transport) {
			return nvme_transport_poll_group_remove(tgroup, qpair);
                                  /* [한국어] 일치 — 트랜스포트별 remove로 위임. tgroup 내부에서 connected/disconnected 리스트에서 제거. */
		}
	}

	return -ENODEV;
                                  /* [한국어] 못 찾음 — 사용자가 잘못된 group/qpair 조합을 줬거나 이미 제거된 상태 */
}

/*
 * [한국어]
 * nvme_qpair_process_completion_wrapper - interrupt 모드에서 qpair fd 트리거 시 호출되는 fd_group 콜백
 *
 * @arg: SPDK_FD_GROUP_ADD_EXT 등록 시 넘긴 qpair 포인터
 * @return spdk_nvme_qpair_process_completions의 반환값 (>0=완료 처리 수, 음수=에러)
 *
 * 호출 경로:
 *   spdk_nvme_poll_group_wait → spdk_fd_group_wait → epoll_wait → 깨어남 →
 *   해당 fd 콜백 = 이 wrapper → 무한대 폴링(0=무제한)으로 qpair 완료 모두 흡수.
 *
 * 0 인자 의미: completions_per_qpair=0이면 "현재 도착한 완료를 모두 처리".
 */
static int
nvme_qpair_process_completion_wrapper(void *arg)
{
	struct spdk_nvme_qpair *qpair = arg;
                                  /* [한국어] void* → qpair 복원 (등록 시 그대로 전달) */

	return spdk_nvme_qpair_process_completions(qpair, 0);
                                  /* [한국어] qpair 단위 완료 폴링 — interrupt 한 번에 가능한 만큼 모두 처리.
                                   *  내부적으로 nvme_qpair.c의 7단계 process_completions가 동작. */
}

/*
 * [한국어]
 * nvme_poll_group_add_qpair_fd - interrupt 모드에서 qpair completion fd를 fd_group에 등록
 *
 * @qpair: 새로 connect된 qpair (qpair->poll_group 이미 설정되어 있음)
 * @return 0 성공 (또는 polling 모드는 즉시 0), 음수: fd 추출 실패 또는 fd_group ADD 실패.
 *
 * 호출 경로: nvme_poll_group_connect_qpair → 트랜스포트 connect 성공 후 → 이 함수.
 *
 * 동작:
 *   [1] enable_interrupts=false이면 return 0 (할 일 없음).
 *   [2] spdk_nvme_qpair_get_fd(qpair, &opts)로 트랜스포트별 completion fd 추출.
 *       PCIe MSI-X는 vfio eventfd, RDMA는 comp_channel fd, TCP는 sock fd 등 트랜스포트마다 의미 다름.
 *       opts에 fd_type 등이 채워져 fd_group이 read 자동 처리 여부를 판단.
 *   [3] SPDK_FD_GROUP_ADD_EXT로 등록 — epoll에 추가되고, 트리거 시
 *       nvme_qpair_process_completion_wrapper(qpair)가 자동 호출됨.
 */
static int
nvme_poll_group_add_qpair_fd(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_poll_group *group;
	struct spdk_event_handler_opts opts = {
		.opts_size = SPDK_SIZEOF(&opts, fd_type),
                                  /* [한국어] ABI 호환 패턴 — 컴파일 시 사용자가 알고 있는 필드까지의 크기를 전달.
                                   *  SPDK_SIZEOF는 구조체 시작부터 fd_type 필드 끝까지의 크기 매크로. */
	};
	int fd;

	group = qpair->poll_group->group;
                                  /* [한국어] qpair가 속한 transport_poll_group → 그 위 spdk_nvme_poll_group 추출.
                                   *  qpair->poll_group은 이전에 spdk_nvme_poll_group_add 시 트랜스포트가 연결해놓음. */
	if (group->enable_interrupts == false) {
                                  /* [한국어] 폴링 모드 — fd 등록할 필요 없음, 모든 폴링은 process_completions에서 명시적으로 수행 */
		return 0;
	}

	fd = spdk_nvme_qpair_get_fd(qpair, &opts);
                                  /* [한국어] 트랜스포트별 completion fd 추출 (vtable의 qpair_get_fd) — interrupt 모드 전용.
                                   *  옵션 출력 인자(opts)에 fd_type 등이 채워짐. */
	if (fd < 0) {
                                  /* [한국어] 트랜스포트가 fd 제공을 지원하지 않거나 내부 에러 */
		NVME_QPAIR_ERRLOG(qpair, "Cannot get fd for the qpair: %d\n", fd);
		return -EINVAL;
	}

	return SPDK_FD_GROUP_ADD_EXT(group->fgrp, fd, nvme_qpair_process_completion_wrapper,
				     qpair, &opts);
                                  /* [한국어] fd_group에 등록 — 이후 epoll_wait가 이 fd 트리거 감지 시 wrapper 자동 호출.
                                   *  wrapper 내부에서 spdk_nvme_qpair_process_completions(qpair, 0). */
}

/*
 * [한국어]
 * nvme_poll_group_remove_qpair_fd - interrupt 모드에서 qpair fd를 fd_group에서 제거
 *
 * @qpair: 제거할 qpair
 *
 * 호출 경로: nvme_poll_group_disconnect_qpair → 이 함수 → 트랜스포트 disconnect.
 *
 * 동작:
 *   [1] polling 모드면 noop.
 *   [2] qpair fd 추출 (NULL opts — 추가 정보 불필요) → fd_group에서 제거.
 *   [3] fd 추출 실패 시 assert (이미 추가됐어야 하는 fd가 사라진 상태는 버그).
 */
static void
nvme_poll_group_remove_qpair_fd(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_poll_group *group;
	int fd;

	group = qpair->poll_group->group;
                                  /* [한국어] add와 동일한 group 추출 경로 */
	if (group->enable_interrupts == false) {
                                  /* [한국어] polling 모드 — fd 자체가 없으므로 즉시 return */
		return;
	}

	fd = spdk_nvme_qpair_get_fd(qpair, NULL);
                                  /* [한국어] 옵션 필요 없으므로 NULL 전달 — fd만 회수.
                                   *  add 때 등록했던 fd와 동일한 값이어야 정상. */
	if (fd < 0) {
                                  /* [한국어] connect 시 add에 성공했다면 fd가 살아 있어야 함 — 음수면 트랜스포트 버그 */
		NVME_QPAIR_ERRLOG(qpair, "Cannot get fd for the qpair: %d\n", fd);
		assert(false);
                                  /* [한국어] 디버그 빌드에서 즉시 abort — 운영 빌드는 단순 return으로 fall through */
		return;
	}

	spdk_fd_group_remove(group->fgrp, fd);
                                  /* [한국어] fd_group(epoll)에서 제거 — 이후 트리거되어도 wrapper 호출 안 됨.
                                   *  실제 fd close는 트랜스포트가 disconnect 처리에서 수행. */
}

/*
 * [한국어]
 * nvme_poll_group_connect_qpair - 트랜스포트 connect + interrupt 모드면 fd_group 등록
 *
 * @qpair: connect할 qpair (이미 spdk_nvme_poll_group_add로 등록된 상태)
 * @return 0 성공, 음수: 트랜스포트 connect 실패 또는 fd_group 등록 실패.
 *
 * 호출 경로:
 *   nvme_ctrlr.c::spdk_nvme_ctrlr_connect_io_qpair
 *     → nvme_transport.c::nvme_transport_ctrlr_connect_qpair
 *       → poll_group이 있으면 이 함수
 *
 * 동작:
 *   [1] 트랜스포트의 poll_group_connect_qpair 호출 — qpair를 connected_qpairs로 이동 등.
 *   [2] interrupt 모드라면 qpair fd를 fd_group에 등록.
 *   [3] fd 등록이 실패하면 트랜스포트 disconnect 롤백 후 에러 반환 (정합성 유지).
 */
int
nvme_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	int rc;

	rc = nvme_transport_poll_group_connect_qpair(qpair);
                                  /* [한국어] 트랜스포트 vtable의 poll_group_connect_qpair 호출.
                                   *  PCIe는 disconnected→connected STAILQ 이동 + num_connected_qpairs++.
                                   *  RDMA/TCP은 추가로 fabric CONNECT 핸드셰이크 시작. */
	if (rc != 0) {
                                  /* [한국어] 트랜스포트가 connect 실패 — 그대로 반환, fd 등록 시도조차 안 함 */
		return rc;
	}

	rc = nvme_poll_group_add_qpair_fd(qpair);
                                  /* [한국어] interrupt 모드일 때만 의미. polling 모드면 즉시 0 반환. */
	if (rc != 0) {
                                  /* [한국어] fd 등록 실패 — connect는 성공했으므로 짝을 맞추기 위해 트랜스포트 disconnect로 롤백.
                                   *  이 보완 처리가 없으면 connected 상태인 qpair가 fd_group 밖에 떠다녀 영영 깨어나지 않음. */
		nvme_transport_poll_group_disconnect_qpair(qpair);
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * nvme_poll_group_disconnect_qpair - interrupt 모드 fd 제거 + 트랜스포트 disconnect
 *
 * @qpair: disconnect할 qpair
 * @return 트랜스포트별 disconnect 콜백의 반환값.
 *
 * 동작 순서가 중요: fd_group 제거를 먼저 해야 disconnect 도중 콜백이 트리거되지 않음.
 */
int
nvme_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	nvme_poll_group_remove_qpair_fd(qpair);
                                  /* [한국어] interrupt 모드면 fd_group에서 제거 (polling 모드면 noop).
                                   *  먼저 제거해야 그 다음 트랜스포트 disconnect 도중 epoll wake로 죽은 qpair 폴링 시도가 일어나지 않음. */

	return nvme_transport_poll_group_disconnect_qpair(qpair);
                                  /* [한국어] 트랜스포트별 disconnect — connected_qpairs에서 disconnected_qpairs로 이동, 카운터 감소 */
}

/*
 * [한국어]
 * spdk_nvme_poll_group_wait - interrupt 모드에서 epoll_wait 호출 (사용자 노출 API)
 *
 * @group: 대상 poll_group
 * @disconnected_qpair_cb: disconnected qpair 발견 시 호출될 사용자 콜백 (NULL 금지)
 * @return num_events (epoll_wait 반환값) 또는 -EINVAL.
 *
 * 동작:
 *   [1] 콜백 NULL 검증.
 *   [2] 모든 tgroup의 disconnected_qpairs 확인 — 이미 끊긴 qpair는 epoll 트리거 없이도 즉시 통지.
 *   [3] spdk_fd_group_wait(timeout=-1로 무한 블로킹) — interrupt 도착할 때까지 대기.
 *
 * 일반적으로 reactor 루프에서:
 *   process_completions로 폴링 → 일이 없으면 wait로 블로킹 → 깨어나면 다시 process_completions.
 *
 * polling 모드 group에서는 fgrp가 없거나 비어있어 즉시 0 반환 — 의미 없음.
 */
int
spdk_nvme_poll_group_wait(struct spdk_nvme_poll_group *group,
			  spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	int num_events, timeout = -1;
                                  /* [한국어] timeout=-1 → epoll_wait 무한 블로킹.
                                   *  현재는 상수지만 변수로 둔 이유는 추후 timeout 인자 추가 가능성 위함. */

	if (disconnected_qpair_cb == NULL) {
                                  /* [한국어] disconnected 통지 콜백은 필수 — NULL이면 통지 누락으로 누수 발생 위험 */
		return -EINVAL;
	}

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
                                  /* [한국어] 모든 트랜스포트 sub-group 순회하며 이미 끊긴 qpair 즉시 통지.
                                   *  wait에 진입하기 전에 처리해야 사용자가 즉각 재연결/제거 결정 가능. */
		nvme_transport_poll_group_check_disconnected_qpairs(tgroup, disconnected_qpair_cb);
                                  /* [한국어] 트랜스포트별로 disconnected_qpairs를 순회하며 사용자 cb 호출 후 리스트에서 제거 */
	}

	num_events = spdk_fd_group_wait(group->fgrp, timeout);
                                  /* [한국어] epoll_wait 호출 — 이벤트 도착 시 자동으로 등록된 콜백 (qpair fd → wrapper,
                                   *  disconnect_qpair_fd → read_disconnect_qpair_fd) 차례로 실행.
                                   *  반환값은 처리한 fd 이벤트 수 (≥0) 또는 음수 errno. */

	return num_events;
                                  /* [한국어] 사용자에게 그대로 전달 — 0이면 일이 없었다는 의미 (timeout=-1이므로 사실상 음수 에러) */
}

/*
 * [한국어]
 * spdk_nvme_poll_group_process_completions - ★ poll_group의 메인 폴링 진입점 ★
 *
 * @group: 대상 poll_group
 * @completions_per_qpair: qpair당 처리할 완료 최대 개수 (0이면 무제한 — "지금 도착한 거 다 처리")
 * @disconnected_qpair_cb: disconnected qpair 발견 시 호출될 사용자 콜백 (NULL 금지)
 * @return 음수: 에러 코드 (-EINVAL 또는 트랜스포트 처리 중 발생한 첫 음수)
 *           0  : 처리한 완료 없음 (일이 없음)
 *           >0 : 처리한 완료 총 개수
 *
 * 사용 패턴 (reactor 루프):
 *   while (true) {
 *     spdk_nvme_poll_group_process_completions(group, 0, my_disconnected_cb);
 *     // 다른 작업
 *   }
 *
 * 동작:
 *   [1] disconnected_qpair_cb NULL 검증.
 *   [2] 재귀 방지 (in_process_completions 플래그) — 사용자 cb_fn 안에서 다시 process_completions를
 *       호출하면 0 반환 (의도치 않은 재귀로 두 번 폴링하는 것 방지).
 *   [3] 모든 tgroup 순회하여 트랜스포트별 process_completions 호출 후 결과 누적.
 *       음수 반환은 첫 번째 것만 보존 (error_reason).
 *       양수는 num_completions에 합산.
 *   [4] 재귀 가드 해제.
 *   [5] error가 있으면 error 반환, 아니면 누적 처리 수 반환.
 *
 * 호출 컨텍스트: poll_group 소유 spdk_thread (단일 스레드성 보장).
 *
 * 호출 체인:
 *   사용자 reactor 루프
 *     → spdk_nvme_poll_group_process_completions (이 함수)
 *       → tgroup별 nvme_transport_poll_group_process_completions
 *         → 트랜스포트 vtable.poll_group_process_completions
 *           → qpair별 spdk_nvme_qpair_process_completions (interrupt 모드는 wrapper 경유)
 *             → 트랜스포트 vtable.qpair_process_completions
 *               → 실제 CQE 폴링 + cb_fn 호출
 */
int64_t
spdk_nvme_poll_group_process_completions(struct spdk_nvme_poll_group *group,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_transport_poll_group *tgroup;
                                  /* [한국어] 순회 중인 트랜스포트별 sub-group */
	int64_t error_reason = 0, num_completions = 0;
                                  /* [한국어] error_reason: 첫 번째 음수 반환 보존 (이후 음수는 무시).
                                   *  num_completions: 양수 반환들의 합계 (사용자가 "이번 호출에서 몇 개 완료됐나"로 활용). */

	if (spdk_unlikely(disconnected_qpair_cb == NULL)) {
                                  /* [한국어] 콜백 누락 — disconnected qpair 누수 위험으로 즉시 거부.
                                   *  spdk_unlikely로 hot-path branch prediction 최적화. */
		return -EINVAL;
	}

	if (spdk_unlikely(group->in_process_completions)) {
                                  /* [한국어] ★ 재귀 가드 ★ — 사용자 cb_fn 내부에서 같은 poll_group의 process_completions를
                                   *  다시 호출하면 0 반환. 의도하지 않은 재진입은 같은 완료를 두 번 처리하거나
                                   *  STAILQ 일관성 깨질 위험. */
		return 0;
	}
	group->in_process_completions = true;
                                  /* [한국어] 가드 활성화 — 이 함수 본체 실행 중에는 재진입 거부 */

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
                                  /* [한국어] 등록된 모든 트랜스포트 sub-group 순회 — PCIe + RDMA + TCP 등 이종 통합 폴링 */
		int64_t local_completions;
                                  /* [한국어] 이번 tgroup이 반환한 완료 수 (또는 음수 에러) */

		local_completions = nvme_transport_poll_group_process_completions(tgroup, completions_per_qpair,
				    disconnected_qpair_cb);
                                  /* [한국어] 트랜스포트별 폴링 위임 — 내부에서 connected_qpairs 순회 + qpair별 process_completions.
                                   *  PCIe는 phase bit 폴링, RDMA는 ibv_poll_cq, TCP는 sock_group_poll 각자 구현. */
		if (spdk_unlikely(local_completions < 0)) {
                                  /* [한국어] 트랜스포트가 에러 반환 — 첫 에러만 보존 (예: -EIO, -ENXIO) */
			if (!error_reason) {
                                  /* [한국어] 아직 에러 미보존 → 이번 것을 기록. 이후 트랜스포트의 에러는 무시 (덮어쓰기 방지). */
				error_reason = local_completions;
			}
		} else {
                                  /* [한국어] 정상 — 누적 */
			num_completions += local_completions;
                                  /* [한국어] 이번 tgroup의 완료 수를 합산 */
			/* Just to be safe */
			assert(num_completions >= 0);
                                  /* [한국어] overflow / 부호 오류 방어 — int64_t에 양수만 누적되므로 음수가 되면 버그 */
		}
	}
	group->in_process_completions = false;
                                  /* [한국어] 가드 해제 — 다음 호출은 정상적으로 진입 가능 */

	return error_reason ? error_reason : num_completions;
                                  /* [한국어] 에러가 있으면 에러 그대로 반환 (사용자에게 진단 신호), 없으면 누적 완료 수 반환.
                                   *  사용자는 음수 분기로 트랜스포트 장애 인지 + 양수로 처리 부하 측정 가능. */
}

/*
 * [한국어]
 * spdk_nvme_poll_group_all_connected - 모든 qpair가 CONNECTED 상태인지 확인 (사용자 노출 API)
 *
 * @group: 대상 poll_group
 * @return 0      : 모두 연결 완료
 *          -EAGAIN: 일부가 아직 CONNECTING 단계 (조금 기다리면 됨)
 *          -EIO   : 일부가 disconnected 또는 비정상 상태 (재연결 필요)
 *
 * 사용 시나리오: 애플리케이션이 모든 qpair connect를 시작한 후, 본격적인 I/O 시작 전에
 *               연결 완료 폴링 — 아직 안 됐으면 process_completions로 진행 → 다시 이 함수 확인.
 *
 * 동작:
 *   모든 tgroup 순회:
 *     - disconnected_qpairs 비어있지 않으면 즉시 -EIO (가장 우선 — 재연결 필요).
 *     - connected_qpairs 순회:
 *         qpair 상태 < CONNECTING (=DISCONNECTED 등)면 -EIO 즉시 반환.
 *         qpair 상태 == CONNECTING이면 rc=-EAGAIN 표시 후 break (이 tgroup 더 안 봐도 됨).
 *
 *   루프 끝까지 -EIO 없이 통과하면 rc(0 또는 -EAGAIN) 반환.
 */
int
spdk_nvme_poll_group_all_connected(struct spdk_nvme_poll_group *group)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	struct spdk_nvme_qpair *qpair;
	int rc = 0;
                                  /* [한국어] 기본은 0(모두 OK) — CONNECTING 발견 시 -EAGAIN으로 격상 */

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
                                  /* [한국어] 트랜스포트별 sub-group 순회 */
		if (!STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
			/* Treat disconnected qpairs as highest priority for notification.
			 * This means we can just return immediately here.
			 */
			return -EIO;
                                  /* [한국어] disconnected 상태가 1개라도 있으면 즉시 -EIO — 사용자에게 재연결 필요 신호 */
		}
		STAILQ_FOREACH(qpair, &tgroup->connected_qpairs, poll_group_stailq) {
                                  /* [한국어] connected_qpairs 리스트(STAILQ)를 순회 — 이름은 "connected"지만 CONNECTING도 여기 들어있음 */
			if (nvme_qpair_get_state(qpair) < NVME_QPAIR_CONNECTING) {
				return -EIO;
                                  /* [한국어] DISCONNECTED 등 CONNECTING 이전 상태(예: 실패 후 fall back) → 회복 불가능 신호 */
			} else if (nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING) {
				rc = -EAGAIN;
                                  /* [한국어] 아직 진행 중 — 사용자에게 "조금 기다려라" 신호.
                                   *  다른 tgroup에 -EIO 후보가 있을 수 있으므로 즉시 return하지 않고 break. */
				/* Break so that we can check the remaining transport groups,
				 * in case any of them have a disconnected qpair.
				 */
				break;
                                  /* [한국어] 이 tgroup에 -EAGAIN 후보가 1개라도 있으면 더 보지 않고 다음 tgroup으로
                                   *  (이 tgroup의 다른 qpair가 모두 CONNECTED여도 결과는 -EAGAIN으로 안 바뀌므로 시간 절약). */
			}
		}
	}

	return rc;
                                  /* [한국어] 0(모두 CONNECTED) 또는 -EAGAIN(일부 CONNECTING) — -EIO는 위에서 즉시 return 됨 */
}

/*
 * [한국어]
 * spdk_nvme_poll_group_get_ctx - 사용자 컨텍스트 getter (사용자 노출 API)
 *
 * @group: 대상 poll_group
 * @return spdk_nvme_poll_group_create의 ctx 인자.
 *
 * 사용처: 사용자 콜백(예: disconnected_qpair_cb) 안에서 자기 컨텍스트로 돌아올 때.
 */
void *
spdk_nvme_poll_group_get_ctx(struct spdk_nvme_poll_group *group)
{
	return group->ctx;
                                  /* [한국어] 단순 필드 반환 — 락/검증 없음 */
}

/*
 * [한국어]
 * spdk_nvme_poll_group_destroy - poll_group 해제 (사용자 노출 API)
 *
 * @group: 해제할 poll_group
 * @return 0 성공, -EBUSY: 트랜스포트 sub-group destroy 실패 (예: 아직 qpair가 남아있음).
 *
 * 동작:
 *   [1] tgroups STAILQ를 SAFE 순회로 빼면서 트랜스포트별 destroy 호출.
 *       destroy 실패하면 빠진 tgroup을 다시 끝에 넣어 일관성 유지하고 -EBUSY 반환
 *       (사용자가 재시도하거나 모든 qpair 빼고 다시 호출 가능).
 *   [2] fgrp가 있다면 (interrupt 모드였다면 disconnect_qpair_fd 먼저 fd_group에서 제거 + close).
 *   [3] fd_group destroy.
 *   [4] group 자체 free.
 *
 * 주의: 이 호출 전에 모든 qpair는 disconnect + remove 되어 있어야 함. 그렇지 않으면 -EBUSY.
 */
int
spdk_nvme_poll_group_destroy(struct spdk_nvme_poll_group *group)
{
	struct spdk_nvme_transport_poll_group *tgroup, *tmp_tgroup;
                                  /* [한국어] STAILQ_FOREACH_SAFE 패턴: tgroup은 현재, tmp_tgroup은 다음 노드 임시 보존 (현재 노드 빼도 안전) */
	struct spdk_fd_group *fgrp = group->fgrp;
                                  /* [한국어] free 전에 fgrp 포인터 보존 — 마지막에 destroy 호출에 사용 */

	STAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp_tgroup) {
                                  /* [한국어] 모든 트랜스포트 sub-group 순회하며 제거 — 빼면서 SAFE라 다음 tmp 미리 잡음 */
		STAILQ_REMOVE(&group->tgroups, tgroup, spdk_nvme_transport_poll_group, link);
                                  /* [한국어] 먼저 group의 리스트에서 제거 — destroy가 실패해도 일관성 회복 가능 */
		if (nvme_transport_poll_group_destroy(tgroup) != 0) {
                                  /* [한국어] 트랜스포트별 destroy 실패 — 보통 아직 qpair가 남아있을 때 */
			STAILQ_INSERT_TAIL(&group->tgroups, tgroup, link);
                                  /* [한국어] 실패한 tgroup을 다시 리스트 끝에 넣어 destroy 가능한 상태로 복원 */
			return -EBUSY;
                                  /* [한국어] 사용자에게 "아직 사용 중이니 정리 후 재시도" 신호 */
		}

	}

	if (fgrp) {
                                  /* [한국어] fd_group이 만들어졌었다면 (Linux + interrupt 모드) 정리 */
		if (group->enable_interrupts) {
                                  /* [한국어] interrupt 모드였다면 disconnect_qpair_fd가 fd_group에 등록되어 있음 */
			spdk_fd_group_remove(fgrp, group->disconnect_qpair_fd);
                                  /* [한국어] fd_group에서 먼저 제거 — close 전에 epoll에서도 빼야 함 */
			close(group->disconnect_qpair_fd);
                                  /* [한국어] eventfd 자체 close — 커널 자원 회수 */
		}
		spdk_fd_group_destroy(fgrp);
                                  /* [한국어] fd_group(epoll fd 포함) 전체 해제 */
	}

	free(group);
                                  /* [한국어] group 구조체 메모리 해제 — 이 시점 이후 group 포인터 사용 금지 */

	return 0;
                                  /* [한국어] 완전 정리 성공 */
}

/*
 * [한국어]
 * spdk_nvme_poll_group_get_stats - 모든 트랜스포트의 통계 수집 (사용자 노출 API)
 *
 * @group: 대상 poll_group
 * @stats: [out] 결과 통계 — 호출자에게 새로 할당된 spdk_nvme_poll_group_stat 포인터 반환.
 *               spdk_nvme_poll_group_free_stats로 반드시 해제 필요.
 * @return 0 성공, -ENOMEM: 메모리 부족, -ENOTSUP: 어떤 트랜스포트도 통계를 지원하지 않음.
 *
 * 동작:
 *   [1] 결과 컨테이너 calloc.
 *   [2] tgroups 길이 = transports_count 측정.
 *   [3] result->transport_stat 배열 크기 transports_count로 calloc.
 *   [4] 각 tgroup의 트랜스포트별 stats 수집 시도 (성공한 것만 카운트 — RDMA만 구현하고 PCIe는 미구현 등).
 *   [5] 하나도 성공 못 했으면 메모리 해제 후 -ENOTSUP.
 *   [6] num_transports에 성공 개수 기록 후 *stats 출력.
 *
 * 주의: result->transport_stat 배열 크기는 transports_count로 잡지만, 실제 채워지는 항목은
 *       reported_stats_count개. 일치하지 않을 수 있으나 사용자는 num_transports만 사용.
 */
int
spdk_nvme_poll_group_get_stats(struct spdk_nvme_poll_group *group,
			       struct spdk_nvme_poll_group_stat **stats)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	struct spdk_nvme_poll_group_stat *result;
	uint32_t transports_count = 0;
                                  /* [한국어] tgroup 개수 — transport_stat 배열 크기 산정용 */
	/* Not all transports used by this poll group may support statistics reporting */
	uint32_t reported_stats_count = 0;
                                  /* [한국어] 실제로 stats 수집에 성공한 트랜스포트 개수 — num_transports로 사용자에게 노출 */
	int rc;

	assert(group);
	assert(stats);
                                  /* [한국어] 두 인자는 NULL이면 안 됨 — 디버그 빌드에서 즉시 abort */

	result = calloc(1, sizeof(*result));
                                  /* [한국어] 결과 컨테이너 — num_transports + transport_stat 배열 포인터로 구성 */
	if (!result) {
		SPDK_ERRLOG("Failed to allocate memory for poll group statistics\n");
		return -ENOMEM;
	}

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
		transports_count++;
                                  /* [한국어] tgroup 개수 카운트 — 첫 번째 순회는 단지 크기 측정용 */
	}

	result->transport_stat = calloc(transports_count, sizeof(*result->transport_stat));
                                  /* [한국어] 트랜스포트별 stats 포인터 배열 — calloc로 NULL 초기화 */
	if (!result->transport_stat) {
		SPDK_ERRLOG("Failed to allocate memory for poll group statistics\n");
		free(result);
                                  /* [한국어] 1단계 성공 후 2단계 실패 → 1단계 메모리 누수 방지 */
		return -ENOMEM;
	}

	STAILQ_FOREACH(tgroup, &group->tgroups, link) {
                                  /* [한국어] 두 번째 순회 — 실제 통계 수집 */
		rc = nvme_transport_poll_group_get_stats(tgroup, &result->transport_stat[reported_stats_count]);
                                  /* [한국어] 트랜스포트 vtable의 poll_group_get_stats 위임.
                                   *  성공 시 transport_stat[reported_stats_count]에 새 stats 포인터 채움. */
		if (rc == 0) {
			reported_stats_count++;
                                  /* [한국어] 성공한 것만 카운트 — 실패한 트랜스포트(미구현 등)는 건너뜀 */
		}
	}

	if (reported_stats_count == 0) {
                                  /* [한국어] 어떤 트랜스포트도 stats를 못 줬음 — 결과 의미 없으므로 모두 해제 후 -ENOTSUP */
		free(result->transport_stat);
		free(result);
		SPDK_DEBUGLOG(nvme, "No transport statistics available\n");
		return -ENOTSUP;
	}

	result->num_transports = reported_stats_count;
                                  /* [한국어] 사용자가 안전하게 순회할 수 있는 개수 */
	*stats = result;
                                  /* [한국어] out 인자에 저장 — 사용자는 free_stats로 해제 책임 */

	return 0;
}

/*
 * [한국어]
 * spdk_nvme_poll_group_free_stats - get_stats 결과 해제 (사용자 노출 API)
 *
 * @group: get_stats를 호출했던 동일 poll_group
 * @stat:  get_stats가 반환한 결과 구조체
 *
 * 동작:
 *   각 transport_stat[i]에 대해 매칭되는 tgroup을 찾아 트랜스포트별 free_stats 위임.
 *   매칭은 trtype(예: PCIE/RDMA/TCP)으로 수행 — get_stats 시점과 free_stats 시점 사이에
 *   tgroup이 추가/삭제되어도 trtype은 동일하다는 가정.
 *   마지막에 transport_stat 배열과 stat 컨테이너 자체 free.
 *
 * 디버그 검증: 모든 stat을 정확히 1번씩 free 했는지 freed_stats == num_transports로 assert.
 */
void
spdk_nvme_poll_group_free_stats(struct spdk_nvme_poll_group *group,
				struct spdk_nvme_poll_group_stat *stat)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	uint32_t i;
	uint32_t freed_stats __attribute__((unused)) = 0;
                                  /* [한국어] release 빌드에서는 assert 컴파일 아웃 → unused 경고 방지 */

	assert(group);
	assert(stat);
                                  /* [한국어] 두 인자 모두 NULL 금지 */

	for (i = 0; i < stat->num_transports; i++) {
                                  /* [한국어] num_transports만큼 순회 — get_stats가 채운 만큼 */
		STAILQ_FOREACH(tgroup, &group->tgroups, link) {
                                  /* [한국어] 매 stat마다 tgroup을 검색 (O(N×M) 비효율이나 stats free는 hot-path 아니므로 OK) */
			if (nvme_transport_get_trtype(tgroup->transport) == stat->transport_stat[i]->trtype) {
                                  /* [한국어] 트랜스포트 타입 일치 — 이 tgroup이 해당 stats를 발급한 곳 */
				nvme_transport_poll_group_free_stats(tgroup, stat->transport_stat[i]);
                                  /* [한국어] 트랜스포트 vtable의 poll_group_free_stats로 위임 — 트랜스포트별 자료구조 해제 */
				freed_stats++;
                                  /* [한국어] 디버그 검증용 카운터 증가 */
				break;
                                  /* [한국어] 매칭 1개 찾으면 다음 stat으로 — 같은 trtype 중복 등록은 가정하지 않음 */
			}
		}
	}

	assert(freed_stats == stat->num_transports);
                                  /* [한국어] 모든 stats가 정확히 free됐는지 검증 — 누락은 트랜스포트가 사라졌거나 trtype 불일치 버그 */

	free(stat->transport_stat);
                                  /* [한국어] 포인터 배열 자체 해제 (배열 항목들은 위에서 트랜스포트별로 해제됨) */
	free(stat);
                                  /* [한국어] 결과 컨테이너 해제 — 이후 stat 포인터 사용 금지 */
}
