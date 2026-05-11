/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SCSI 라이브러리 내부 정의 헤더 (scsi_internal.h)
 *
 * === 파일의 역할 ===
 * lib/scsi의 모든 .c 파일이 공유하는 내부 자료구조와 함수 프로토타입을 정의하는 private 헤더.
 * 외부 노출 API는 include/spdk/scsi.h에 있으며, 본 헤더는 라이브러리 내부 구현 세부에만 쓰인다.
 * 정의 항목:
 *   - 상수: SPDK_SCSI_DEV_MAX_LUN, SCSI_SPC2_RESERVE 플래그.
 *   - enum: 명령 처리 결과(SPDK_SCSI_TASK_PENDING/COMPLETE/UNKNOWN).
 *   - 구조체: spdk_scsi_port, spdk_scsi_pr_registrant, spdk_scsi_pr_reservation,
 *             spdk_scsi_dev, spdk_scsi_lun_desc, spdk_scsi_lun.
 *   - 내부 함수 프로토타입(static 아닌 파일간 공유): scsi_lun_*, scsi_dev_get_list,
 *     scsi_port_construct/destruct, bdev_scsi_execute/reset, scsi_pr_*, scsi2_*.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK SCSI target은 이렇게 계층화되어 있다:
 *     iSCSI / vhost-scsi (frontend, lib/iscsi, lib/vhost)
 *         │ task 생성/제출 / 응답 수신
 *         ▼
 *     SPDK SCSI core (본 디렉터리 lib/scsi)
 *         │ task 큐잉, PR 검사, sense 빌드, LUN 관리
 *         ▼
 *     SCSI bdev backend (bdev_scsi_execute → bdev I/O)
 *         │
 *         ▼
 *     SPDK bdev layer (lib/bdev)
 * 본 헤더의 구조체는 위 계층에서 SCSI core 영역의 핵심 객체 그래프(dev → lun → task)를 형성한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h(libc 표준), spdk/bdev.h(spdk_bdev/desc/io_channel), spdk/scsi.h(공개 타입),
 *         spdk/scsi_spec.h(SCSI 와이어 포맷·opcode·sense 코드 enum), spdk/trace.h, spdk/dif.h, spdk/log.h.
 * - 사용처: dev.c, lun.c, port.c, scsi.c, scsi_pr.c, scsi_rpc.c, task.c와 lib/scsi/scsi_bdev.c.
 *           프론트엔드(lib/iscsi, lib/vhost)는 본 헤더가 아닌 공개 헤더(spdk/scsi.h)만 사용.
 * - 객체 그래프: spdk_scsi_dev ─*→ spdk_scsi_lun ─*→ tasks/pending_tasks/mgmt_*/reg_head ;
 *                spdk_scsi_lun ─*→ spdk_scsi_lun_desc(opener) ;
 *                spdk_scsi_lun → bdev/bdev_desc/io_channel (lib/bdev 객체).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_scsi_port: SCSI 포트(initiator/target endpoint) — id/index/name/transport_id.
 * - spdk_scsi_pr_registrant: I_T nexus당 PR 등록자(rkey, port 포인터, transport_id).
 * - spdk_scsi_pr_reservation: LUN당 reservation 상태(holder, type, crkey, SPC2 flag).
 * - spdk_scsi_dev: SCSI device — LUN의 컬렉션 + 포트 배열. dev.c의 g_devs[]에 저장.
 * - spdk_scsi_lun_desc: LUN을 연 사용자별 핸들 — hotremove 콜백 보관.
 * - spdk_scsi_lun: 한 LUN의 모든 상태 — bdev/desc/io_channel/task 큐/PR 등록자/예약.
 * - scsi_lun_*(): LUN 단위 task 실행/완료/관리 함수들(lun.c가 구현).
 * - scsi_pr_*(), scsi2_*(): SCSI Persistent Reservation 처리(scsi_pr.c).
 * - bdev_scsi_execute/reset: SCSI CDB → bdev I/O로 변환(scsi_bdev.c).
 */

#ifndef SPDK_SCSI_INTERNAL_H
#define SPDK_SCSI_INTERNAL_H
/* [한국어] include guard — 헤더 중복 포함 방지. */

#include "spdk/stdinc.h"
/* [한국어] 표준 C 라이브러리 일괄 포함(uint*_t, size_t, memset 등). */

#include "spdk/bdev.h"
/* [한국어] spdk_bdev / spdk_bdev_desc / spdk_bdev_io / spdk_io_channel 등 — LUN이 보관할 블록 디바이스 객체. */
#include "spdk/scsi.h"
/* [한국어] 공개 SCSI API 타입(spdk_scsi_task, spdk_scsi_dev_destruct_cb_t 등). */
#include "spdk/scsi_spec.h"
/* [한국어] SCSI 와이어 포맷/opcode/sense 코드/Persistent Reservation 타입 enum. */
#include "spdk/trace.h"
/* [한국어] SPDK trace 프레임워크 — TRACE_SCSI_TASK_START/DONE 매크로. */
#include "spdk/dif.h"
/* [한국어] DIF(Data Integrity Field) 컨텍스트 — bdev_scsi_get_dif_ctx에서 사용. */

#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG / SPDK_DEBUGLOG / SPDK_LOG_REGISTER_COMPONENT 매크로. */

#define SPDK_SCSI_DEV_MAX_LUN	256
/* [한국어] 한 spdk_scsi_dev가 가질 수 있는 최대 LUN 개수. SAM-3의 256-LUN 한계와 일치. */

/*
 * [한국어]
 * SCSI task 처리 결과 코드(내부) — bdev_scsi_execute / scsi_lun 분기에서 사용
 */
enum {
	SPDK_SCSI_TASK_UNKNOWN = -1,
	/* [한국어] 분기에서 결정되지 않은 초기값 — 외부에서 직접 사용되진 않으나 디버깅 안전망.
	 * 설정자: 일부 헬퍼의 초기값.
	 * 읽는 자: lun.c의 switch 분기에서 default → abort()로 fail-fast.
	 * 값 범위: -1.
	 * 동기화: 단일 task 내부 값이므로 동기화 불필요. */
	SPDK_SCSI_TASK_COMPLETE,
	/* [한국어] task가 동기적으로 완료되었음 — 호출자(lun.c)가 즉시 cpl_fn을 호출해야 한다.
	 * 설정자: bdev_scsi_execute / scsi_pr_check / 예약 충돌 분기 / hot-remove abort 분기.
	 * 읽는 자: _scsi_lun_execute_task의 switch 문.
	 * 값 범위: 0.
	 * 동기화: task 단일 스레드 처리이므로 lock 불필요. */
	SPDK_SCSI_TASK_PENDING,
	/* [한국어] task가 비동기로 진행 중 — bdev I/O 콜백이 나중에 완료를 알릴 때까지 호출자는 대기.
	 * 설정자: bdev_scsi_execute가 bdev I/O 제출 후 반환할 때.
	 * 읽는 자: _scsi_lun_execute_task의 switch 문 (case PENDING은 break로 종료).
	 * 값 범위: 1.
	 * 동기화: task 단일 스레드 처리이므로 lock 불필요. */
};

/*
 * [한국어]
 * struct spdk_scsi_port - SCSI 포트(initiator/target endpoint) 추상화
 *
 * SCSI 표준의 "port"는 SCSI 명령이 들어오고 나가는 종단점이다. 본 구조체는 port 한 개의
 * 정체성(id/index/name)과 옵션 TransportID를 담는다. dev 내장 배열의 슬롯 또는 동적 객체로 사용된다.
 */
struct spdk_scsi_port {
	uint8_t			is_used;
	/* [한국어] dev 내장 배열에서 슬롯 사용 여부 플래그.
	 * 설정자: scsi_port_construct(=1), scsi_port_destruct(memset으로 0).
	 * 읽는 자: scsi_dev_find_free_port, spdk_scsi_dev_find_port_by_id.
	 * 값 범위: 0(빈 슬롯), 1(사용 중).
	 * 동기화: dev는 단일 관리 스레드(보통 메인)에서만 다룸 → lock 불필요. */
	uint64_t		id;
	/* [한국어] port 64비트 식별자(상위 모듈이 부여 — 예: dev 내 인덱스, 시스템 전역 ID).
	 * 설정자: scsi_port_construct.
	 * 읽는 자: spdk_scsi_dev_find_port_by_id 매칭 키.
	 * 값 범위: 호출자 정의(0 허용).
	 * 동기화: 위와 동일. */
	uint16_t		index;
	/* [한국어] dev 내 port 인덱스 — SCSI 표준의 "relative target port id"로 사용.
	 * 설정자: scsi_port_construct(보통 dev->num_ports).
	 * 읽는 자: scsi_pr_register_registrant가 reg->relative_target_port_id로 복사.
	 * 값 범위: [0, SPDK_SCSI_DEV_MAX_PORTS).
	 * 동기화: 위와 동일. */
	uint16_t		transport_id_len;
	/* [한국어] transport_id 바이트 배열의 유효 길이.
	 * 설정자: spdk_scsi_port_set_iscsi_transport_id (= sizeof(헤더)+이름길이).
	 * 읽는 자: scsi_pr_register_registrant, scsi_pr_in_read_full_status.
	 * 값 범위: 0(미설정) 또는 [최소 SPC-3 길이, SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH].
	 * 동기화: port 소유 스레드 단일. */
	char			transport_id[SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH];
	/* [한국어] SPC-3 TransportID 와이어 바이트(헤더 + 가변 이름 + 패딩).
	 * 설정자: spdk_scsi_port_set_iscsi_transport_id (in-place 인코딩).
	 * 읽는 자: scsi_pr_* 가 PR 등록자 비교/READ FULL STATUS 응답에 사용.
	 * 값 범위: 첫 바이트는 protocol_id(SPC), 이후 format/길이 인코딩 의미.
	 * 동기화: 위와 동일. */
	char			name[SPDK_SCSI_PORT_MAX_NAME_LENGTH];
	/* [한국어] 사람이 읽는 이름(예: "iqn.host"). 로그/RPC 응답에 사용.
	 * 설정자: scsi_port_construct(snprintf로 안전 복사).
	 * 읽는 자: spdk_scsi_port_get_name, scsi_pr.c (registrant->initiator_port_name 복사).
	 * 값 범위: 널 종료 문자열, 최대 길이 = SPDK_SCSI_PORT_MAX_NAME_LENGTH-1.
	 * 동기화: 위와 동일. */
};

/* Registrant with I_T nextus */
/*
 * [한국어]
 * struct spdk_scsi_pr_registrant - I_T nexus(이니시에이터-타겟 페어)당 PR 등록자
 *
 * SCSI Persistent Reservation(SPC-4 5.6)에서 reservation을 등록한 I_T nexus 한 개를 표현.
 * 한 LUN의 reg_head 리스트에 매달려 관리된다.
 */
struct spdk_scsi_pr_registrant {
	uint64_t				rkey;
	/* [한국어] PR Reservation Key — 64비트.
	 * 설정자: scsi_pr_out_register / replace_registrant_key.
	 * 읽는 자: PR 충돌 검사 (scsi_pr_check, scsi_pr_out_*), READ KEYS 응답.
	 * 값 범위: 호출자 부여(0 포함). 0이면 "key 없음" 의미로 쓰일 수 있음.
	 * 동기화: LUN 단일 스레드(lun->thread)에서만 reg_head 조작 → lock 불필요. */
	uint16_t				relative_target_port_id;
	/* [한국어] 등록 시점의 target_port->index 복사본 — 응답에 그대로 사용.
	 * 설정자: scsi_pr_register_registrant.
	 * 읽는 자: scsi_pr_in_read_full_status.
	 * 값 범위: target_port의 index 값.
	 * 동기화: 위와 동일. */
	uint16_t				transport_id_len;
	/* [한국어] 아래 transport_id의 유효 길이.
	 * 설정자: scsi_pr_register_registrant (initiator_port의 transport_id_len 복사).
	 * 읽는 자: scsi_pr_in_read_full_status.
	 * 값 범위: 0~SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH.
	 * 동기화: 위와 동일. */
	char					transport_id[SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH];
	/* [한국어] 등록 시점의 initiator_port->transport_id 복사본 — port가 사라져도 PR이 살아있게.
	 * 설정자: scsi_pr_register_registrant.
	 * 읽는 자: scsi_pr_in_read_full_status (응답에 그대로 출력).
	 * 값 범위: SPC-3 인코딩 바이트열.
	 * 동기화: 위와 동일. */
	char					initiator_port_name[SPDK_SCSI_PORT_MAX_NAME_LENGTH];
	/* [한국어] 등록 시점 initiator port 이름 복사.
	 * 설정자: scsi_pr_register_registrant.
	 * 읽는 자: 디버그 로그.
	 * 값 범위: 널 종료 문자열.
	 * 동기화: 위와 동일. */
	char					target_port_name[SPDK_SCSI_PORT_MAX_NAME_LENGTH];
	/* [한국어] 등록 시점 target port 이름 복사.
	 * 설정자: scsi_pr_register_registrant.
	 * 읽는 자: 디버그 로그.
	 * 값 범위: 널 종료 문자열.
	 * 동기화: 위와 동일. */
	struct spdk_scsi_port			*initiator_port;
	/* [한국어] 등록한 initiator port 포인터(I_T nexus의 I 측).
	 * 설정자: scsi_pr_register_registrant.
	 * 읽는 자: scsi_pr_get_registrant 매칭 (현재 task의 initiator_port와 비교).
	 * 값 범위: 유효 spdk_scsi_port * 또는 NULL(이름 없는 등록 시).
	 * 동기화: port 자체는 별도 라이프사이클 — 보통 LUN과 동일한 scope에서 살아있음. */
	struct spdk_scsi_port			*target_port;
	/* [한국어] 등록한 target port 포인터(I_T nexus의 T 측).
	 * 설정자: scsi_pr_register_registrant.
	 * 읽는 자: scsi_pr_get_registrant 매칭.
	 * 값 범위: 유효 spdk_scsi_port * 또는 NULL.
	 * 동기화: 위와 동일. */
	TAILQ_ENTRY(spdk_scsi_pr_registrant)	link;
	/* [한국어] lun->reg_head에 매달리기 위한 BSD TAILQ 링크 노드.
	 * 설정자: TAILQ_INSERT_TAIL (등록 시), TAILQ_REMOVE (해제 시).
	 * 읽는 자: TAILQ_FOREACH 순회 (PR check/응답 빌드).
	 * 값 범위: queue.h 매크로 관리.
	 * 동기화: lun 단일 스레드에서만 조작. */
};

#define SCSI_SPC2_RESERVE			0x00000001U
/* [한국어] reservation.flags의 비트 — SPC-2 RESERVE(6)/RESERVE(10)으로 점유된 상태 표시.
 *          PR(SPC-3)과 SPC-2 reserve를 동일 구조체에서 함께 표현하기 위한 모드 비트. */

/* Reservation with LU_SCOPE */
/*
 * [한국어]
 * struct spdk_scsi_pr_reservation - LUN당 현재 활성화된 reservation 상태
 *
 * 한 LUN은 동시에 0 또는 1개의 reservation을 가진다(LU_SCOPE 한정). PR(SPC-3) 또는
 * SPC-2 RESERVE 둘 중 하나의 모드로 운용되며, 본 구조체가 holder/type/key를 보관한다.
 */
struct spdk_scsi_pr_reservation {
	uint32_t				flags;
	/* [한국어] reservation 모드 플래그(현재는 SCSI_SPC2_RESERVE 비트만 정의).
	 * 설정자: scsi2_reserve(=1로 셋), scsi2_release(memset으로 0), PR 코드(0).
	 * 읽는 자: _scsi_lun_execute_task가 SPC-2/PR 검사를 분기.
	 * 값 범위: 0 또는 SCSI_SPC2_RESERVE.
	 * 동기화: lun 단일 스레드. */
	struct spdk_scsi_pr_registrant		*holder;
	/* [한국어] 현재 reservation을 잡고 있는 등록자 포인터(없으면 NULL).
	 *          all-registrants 타입에서는 임의의 등록자 한 명을 holder로 둔다(편의상).
	 * 설정자: scsi_pr_reserve_reservation, scsi2_reserve(=&lun->scsi2_holder).
	 * 읽는 자: scsi_pr_check, scsi_pr_in_*, scsi2_*.
	 * 값 범위: NULL 또는 reg_head/scsi2_holder 안의 유효 포인터.
	 * 동기화: lun 단일 스레드. */
	enum spdk_scsi_pr_type_code		rtype;
	/* [한국어] reservation type 코드(WRITE_EXCLUSIVE, EXCLUSIVE_ACCESS, *_REGS_ONLY, *_ALL_REGS).
	 * 설정자: scsi_pr_reserve_reservation, PR preempt.
	 * 읽는 자: scsi_pr_check (read/write 허용 결정), scsi_pr_in_read_reservations.
	 * 값 범위: enum spdk_scsi_pr_type_code (scsi_spec.h).
	 * 동기화: lun 단일 스레드. */
	uint64_t				crkey;
	/* [한국어] current reservation key — holder의 rkey와 동일(편의 캐시).
	 * 설정자: scsi_pr_reserve_reservation, replace_registrant_key 등.
	 * 읽는 자: scsi_pr_out_release/preempt 키 매칭, READ RESERVATIONS 응답.
	 * 값 범위: 64비트 임의값.
	 * 동기화: lun 단일 스레드. */
};

/*
 * [한국어]
 * struct spdk_scsi_dev - SCSI device(LUN의 컬렉션)
 *
 * dev.c의 g_devs[SPDK_SCSI_MAX_DEVS] 정적 배열의 슬롯이며, 한 dev는 여러 LUN을 가진다.
 * 프론트엔드(iSCSI Target/vhost-scsi)에서 1개의 LUN 컬렉션을 묶는 단위로 사용된다.
 */
struct spdk_scsi_dev {
	int					id;
	/* [한국어] dev 슬롯 인덱스(=g_devs[] 배열 인덱스).
	 * 설정자: allocate_dev (=i).
	 * 읽는 자: spdk_scsi_dev_get_id, RPC 응답.
	 * 값 범위: [0, SPDK_SCSI_MAX_DEVS).
	 * 동기화: dev 풀은 단일 스레드(메인)에서만 조작. */
	int					is_allocated;
	/* [한국어] 슬롯 사용 중 여부 — allocate_dev/free_dev가 토글.
	 * 설정자: allocate_dev(=1), free_dev(=0).
	 * 읽는 자: allocate_dev/스캔, RPC.
	 * 값 범위: 0/1.
	 * 동기화: 위와 동일. */
	bool					removed;
	/* [한국어] destruct 진행 중 표시 — 모든 LUN이 빠질 때까지 대기 중인 상태.
	 * 설정자: spdk_scsi_dev_destruct (=true).
	 * 읽는 자: free_dev(assert), spdk_scsi_dev_delete_lun(빈 LUN 시 free_dev 트리거).
	 * 값 범위: false/true.
	 * 동기화: 위와 동일. */
	spdk_scsi_dev_destruct_cb_t		remove_cb;
	/* [한국어] destruct 완료 통지 콜백.
	 * 설정자: spdk_scsi_dev_destruct.
	 * 읽는 자: free_dev (호출 후 NULL로 클리어).
	 * 값 범위: 함수 포인터 또는 NULL(콜백 미지정).
	 * 동기화: 위와 동일. */
	void					*remove_ctx;
	/* [한국어] remove_cb의 user context.
	 * 설정자: spdk_scsi_dev_destruct.
	 * 읽는 자: free_dev.
	 * 값 범위: 임의 포인터.
	 * 동기화: 위와 동일. */

	char					name[SPDK_SCSI_DEV_MAX_NAME + 1];
	/* [한국어] device 이름(널 종료, +1은 null terminator 자리).
	 * 설정자: spdk_scsi_dev_construct_ext (memcpy).
	 * 읽는 자: spdk_scsi_dev_get_name, RPC.
	 * 값 범위: 널 종료 ASCII 문자열.
	 * 동기화: 위와 동일. */

	TAILQ_HEAD(, spdk_scsi_lun)		luns;
	/* [한국어] 이 dev에 속한 LUN 리스트의 head — lun.tailq 링크로 매달림.
	 * 설정자: allocate_dev(TAILQ_INIT), spdk_scsi_dev_add_lun_ext(insert),
	 *         spdk_scsi_dev_delete_lun(remove).
	 * 읽는 자: spdk_scsi_dev_get_lun, scsi_dev_find_free_lun, has_pending_tasks 등 다수.
	 * 값 범위: 0개~SPDK_SCSI_DEV_MAX_LUN개의 LUN.
	 * 동기화: 위와 동일. */

	int					num_ports;
	/* [한국어] 현재 사용 중인 포트 수.
	 * 설정자: spdk_scsi_dev_add_port (++), spdk_scsi_dev_delete_port (--).
	 * 읽는 자: add_port의 max 검사, 새 port의 index로 사용.
	 * 값 범위: [0, SPDK_SCSI_DEV_MAX_PORTS].
	 * 동기화: 위와 동일. */
	struct spdk_scsi_port			port[SPDK_SCSI_DEV_MAX_PORTS];
	/* [한국어] 내장 port 슬롯 배열(고정 크기). is_used로 빈/쓰는 슬롯 구분.
	 * 설정자: scsi_port_construct/destruct.
	 * 읽는 자: scsi_dev_find_free_port, spdk_scsi_dev_find_port_by_id.
	 * 값 범위: 슬롯 단위.
	 * 동기화: 위와 동일. */

	uint8_t					protocol_id;
	/* [한국어] device의 SCSI protocol identifier(SPC-3 Table 244): iSCSI=0x05, FC=0x00 등.
	 * 설정자: spdk_scsi_dev_construct_ext.
	 * 읽는 자: 프론트엔드(INQUIRY 응답 등).
	 * 값 범위: SPDK_SPC_PROTOCOL_IDENTIFIER_*.
	 * 동기화: 위와 동일. */
};

/*
 * [한국어]
 * struct spdk_scsi_lun_desc - LUN을 연 사용자(opener)의 개별 핸들
 *
 * 한 LUN을 여러 사용자가 동시에 열 수 있고, 각자 hot-remove 콜백을 가질 수 있다.
 * spdk_scsi_lun_open 시 desc 한 개가 lun->open_descs에 매달리고, close 시 제거된다.
 */
struct spdk_scsi_lun_desc {
	struct spdk_scsi_lun		*lun;
	/* [한국어] 이 desc가 가리키는 LUN.
	 * 설정자: spdk_scsi_lun_open.
	 * 읽는 자: spdk_scsi_lun_close, spdk_scsi_lun_allocate_io_channel 등.
	 * 값 범위: 유효 LUN 포인터(NULL 불가).
	 * 동기화: LUN 단일 스레드. */
	spdk_scsi_lun_remove_cb_t	hotremove_cb;
	/* [한국어] LUN hot-remove 시 이 opener에게 알리는 콜백.
	 * 설정자: spdk_scsi_lun_open.
	 * 읽는 자: scsi_lun_notify_hot_remove (NULL이면 자동 close).
	 * 값 범위: 함수 포인터 또는 NULL.
	 * 동기화: 위와 동일. */
	void				*hotremove_ctx;
	/* [한국어] hotremove_cb의 user context.
	 * 설정자: spdk_scsi_lun_open.
	 * 읽는 자: scsi_lun_notify_hot_remove.
	 * 값 범위: 임의 포인터.
	 * 동기화: 위와 동일. */
	TAILQ_ENTRY(spdk_scsi_lun_desc)	link;
	/* [한국어] lun->open_descs 리스트 매달기용 링크.
	 * 설정자: TAILQ_INSERT_TAIL/REMOVE.
	 * 읽는 자: TAILQ_FOREACH (hot-remove broadcast).
	 * 값 범위: queue.h 관리.
	 * 동기화: 위와 동일. */
};

/*
 * [한국어]
 * struct spdk_scsi_lun - 한 SCSI LUN(Logical Unit)의 모든 상태
 *
 * SCSI LUN은 결국 SPDK bdev 한 개에 대응되며, 본 구조체가 그 bdev 핸들/IO 채널/예약/등록자/task 큐를
 * 모두 보관한다. LUN은 단일 SPDK 스레드(lun->thread)에 고정되며, 모든 task는 그 스레드에서 실행된다.
 * task 큐는 두 단계(pending → tasks)로 운용되어, mgmt task와 IO task가 직렬화되도록 보장한다.
 */
struct spdk_scsi_lun {
	/** LUN id for this logical unit. */
	int id;
	/* [한국어] dev 내 LUN 인덱스(예: 0, 1, 2...). LUN 0이 반드시 존재해야 한다(SPC-4).
	 * 설정자: spdk_scsi_dev_add_lun_ext (자동 또는 명시).
	 * 읽는 자: spdk_scsi_dev_get_lun(매칭), REPORT LUNS 응답.
	 * 값 범위: [0, SPDK_SCSI_DEV_MAX_LUN).
	 * 동기화: dev 관리 스레드. */

	/** The LUN is removed */
	bool removed;
	/* [한국어] hot-remove 진행 중/완료 표시 — 새 task는 abort, 기존 task는 처리 후 완료 대기.
	 * 설정자: scsi_lun_hot_remove(=true).
	 * 읽는 자: _scsi_lun_execute_task, _scsi_lun_execute_mgmt_task, spdk_scsi_lun_is_removing.
	 * 값 범위: false/true.
	 * 동기화: lun->thread 단일. */

	/** The LUN is resizing */
	bool resizing;
	/* [한국어] bdev RESIZE 이벤트 수신 후 첫 명령 시 UNIT ATTENTION을 띄우기 위한 1회용 플래그.
	 * 설정자: bdev_event_cb(=true), _scsi_lun_execute_task(=false: UA 띄운 직후 클리어).
	 * 읽는 자: _scsi_lun_execute_task.
	 * 값 범위: false/true.
	 * 동기화: lun->thread 단일. */

	/** Pointer to the SCSI device containing this LUN. */
	struct spdk_scsi_dev *dev;
	/* [한국어] 이 LUN을 소유한 dev 포인터.
	 * 설정자: spdk_scsi_dev_add_lun_ext (lun->dev = dev).
	 * 읽는 자: scsi_lun_complete_task가 dev->id를 trace에, _scsi_lun_remove → spdk_scsi_dev_delete_lun.
	 * 값 범위: 유효 dev 포인터(NULL 불가, LUN이 dev에 속해 있는 동안).
	 * 동기화: dev 관리 스레드와 동일 컨텍스트에서 설정. */

	/** The bdev associated with this LUN. */
	struct spdk_bdev *bdev;
	/* [한국어] 이 LUN이 매핑된 SPDK bdev 객체.
	 * 설정자: scsi_lun_construct (spdk_bdev_desc_get_bdev).
	 * 읽는 자: bdev_scsi_execute, spdk_scsi_lun_get_bdev_name 등.
	 * 값 범위: 유효 bdev 포인터(LUN이 살아있는 동안).
	 * 동기화: 본 LUN 스레드에서만 사용. */

	/** Descriptor for opened block device. */
	struct spdk_bdev_desc *bdev_desc;
	/* [한국어] spdk_bdev_open_ext로 얻은 디스크립터 — close 시 동일 desc 사용.
	 * 설정자: scsi_lun_construct.
	 * 읽는 자: io_channel 획득(spdk_bdev_get_io_channel), close 시 spdk_bdev_close.
	 * 값 범위: 유효 desc 포인터.
	 * 동기화: 본 LUN 스레드. */

	/** The thread which opens this LUN. */
	struct spdk_thread *thread;
	/* [한국어] LUN을 소유한 SPDK 스레드 — 모든 LUN 조작은 이 스레드에서 실행되어야 함.
	 * 설정자: scsi_lun_construct (=spdk_get_thread()).
	 * 읽는 자: scsi_lun_remove (spdk_thread_exec_msg 대상), 디버그.
	 * 값 범위: 유효 spdk_thread *.
	 * 동기화: 한번 정해지면 LUN 라이프사이클 동안 불변 → 락 불필요. */

	/** I/O channel for the bdev associated with this LUN. */
	struct spdk_io_channel *io_channel;
	/* [한국어] bdev I/O 채널 — bdev I/O는 이 채널을 통해 제출.
	 * 설정자: scsi_lun_allocate_io_channel.
	 * 읽는 자: bdev_scsi_execute, scsi_lun_check_io_channel.
	 * 값 범위: 유효 채널 또는 NULL(아직 미할당 또는 free 후).
	 * 동기화: 채널은 그것이 만들어진 스레드 컨텍스트에서만 사용 가능 (SPDK 규칙). */

	/** Poller to release the resource of the lun when it is hot removed */
	struct spdk_poller *hotremove_poller;
	/* [한국어] hot-remove 진행 시 outstanding task가 모두 끝날 때까지 폴링하는 SPDK poller.
	 * 설정자: scsi_lun_check_outstanding_tasks 등록 시점, scsi_lun_check_io_channel 등록 시점.
	 * 읽는 자: spdk_poller_unregister.
	 * 값 범위: 유효 poller 또는 NULL(미등록).
	 * 동기화: lun->thread. */

	/** Callback to be fired when LUN removal is first triggered. */
	void (*hotremove_cb)(const struct spdk_scsi_lun *lun, void *arg);
	/* [한국어] LUN 제거가 시작될 때 LUN 자체에 대해 한 번 부를 콜백.
	 * 설정자: scsi_lun_construct(생성자에서 등록).
	 * 읽는 자: scsi_lun_notify_hot_remove.
	 * 값 범위: 함수 포인터 또는 NULL.
	 * 동기화: lun->thread. */

	/** Argument for hotremove_cb */
	void *hotremove_ctx;
	/* [한국어] hotremove_cb의 user context.
	 * 설정자: scsi_lun_construct.
	 * 읽는 자: scsi_lun_notify_hot_remove.
	 * 값 범위: 임의 포인터.
	 * 동기화: lun->thread. */

	/** Callback to be fired when the bdev size of related LUN has changed. */
	void (*resize_cb)(const struct spdk_scsi_lun *, void *);
	/* [한국어] bdev RESIZE 이벤트를 상위(iSCSI 등)에 통지하기 위한 콜백.
	 * 설정자: scsi_lun_construct.
	 * 읽는 자: bdev_event_cb (RESIZE 분기에서 호출).
	 * 값 범위: 함수 포인터 또는 NULL.
	 * 동기화: lun->thread. */

	/** Argument for resize_cb */
	void *resize_ctx;
	/* [한국어] resize_cb의 user context.
	 * 설정자: scsi_lun_construct.
	 * 읽는 자: bdev_event_cb.
	 * 값 범위: 임의 포인터.
	 * 동기화: lun->thread. */

	/** List of open descriptors for this LUN. */
	TAILQ_HEAD(, spdk_scsi_lun_desc) open_descs;
	/* [한국어] spdk_scsi_lun_open()으로 만든 desc들의 리스트.
	 * 설정자: spdk_scsi_lun_open(insert), spdk_scsi_lun_close(remove).
	 * 읽는 자: scsi_lun_notify_hot_remove (모든 opener에게 통지).
	 * 값 범위: 0개 이상의 desc.
	 * 동기화: lun->thread. */

	/** submitted tasks */
	TAILQ_HEAD(tasks, spdk_scsi_task) tasks;
	/* [한국어] 이미 bdev로 제출된(또는 즉시 완료될) IO task의 리스트.
	 * 설정자: _scsi_lun_execute_task가 INSERT_TAIL, scsi_lun_complete_task가 REMOVE.
	 * 읽는 자: scsi_lun_has_outstanding_tasks, scsi_lun_has_pending_tasks(initiator 매칭).
	 * 값 범위: 0개 이상.
	 * 동기화: lun->thread. */

	/** pending tasks */
	TAILQ_HEAD(pending_tasks, spdk_scsi_task) pending_tasks;
	/* [한국어] mgmt task가 진행 중이거나 직렬화 대기 중인 IO task 큐.
	 * 설정자: scsi_lun_append_task, scsi_lun_execute_tasks가 REMOVE 후 _scsi_lun_execute_task로.
	 * 읽는 자: _scsi_lun_has_pending_tasks.
	 * 값 범위: 0개 이상.
	 * 동기화: lun->thread. */

	/** submitted management tasks */
	TAILQ_HEAD(mgmt_tasks, spdk_scsi_task) mgmt_tasks;
	/* [한국어] 현재 진행 중(submitted)인 mgmt task — 동시 1개만 허용되도록 운용.
	 * 설정자: _scsi_lun_execute_mgmt_task가 INSERT, scsi_lun_complete_mgmt_task가 REMOVE.
	 * 읽는 자: _scsi_lun_execute_mgmt_task의 진입 가드, scsi_lun_has_outstanding_mgmt_tasks.
	 * 값 범위: 0~1개.
	 * 동기화: lun->thread. */

	/** pending management tasks */
	TAILQ_HEAD(pending_mgmt_tasks, spdk_scsi_task) pending_mgmt_tasks;
	/* [한국어] 처리 대기 중인 mgmt task FIFO.
	 * 설정자: scsi_lun_append_mgmt_task, _scsi_lun_execute_mgmt_task가 dequeue.
	 * 읽는 자: _scsi_lun_has_pending_mgmt_tasks.
	 * 값 범위: 0개 이상.
	 * 동기화: lun->thread. */

	/** poller to check completion of tasks prior to reset */
	struct spdk_poller *reset_poller;
	/* [한국어] LUN reset/Target reset task가 outstanding IO 완료를 폴링 대기하는 poller.
	 * 설정자: scsi_lun_complete_reset_task가 등록.
	 * 읽는 자: scsi_lun_reset_check_outstanding_tasks가 unregister.
	 * 값 범위: 유효 poller 또는 NULL.
	 * 동기화: lun->thread. */

	/** A structure to connect LUNs in a list. */
	TAILQ_ENTRY(spdk_scsi_lun) tailq;
	/* [한국어] dev->luns 리스트에 매달리기 위한 링크.
	 * 설정자: spdk_scsi_dev_add_lun_ext / delete_lun.
	 * 읽는 자: TAILQ_FOREACH (find_lun, get_first/next, has_pending_tasks).
	 * 값 범위: queue.h 관리.
	 * 동기화: dev 관리 스레드. */

	/**  The reference number for this LUN, thus we can correctly free the io_channel */
	uint32_t ref;
	/* [한국어] io_channel 사용자 수 — 0이 되면 채널을 spdk_put_io_channel.
	 * 설정자: scsi_lun_allocate_io_channel(++), scsi_lun_free_io_channel(--).
	 * 읽는 자: scsi_lun_free_io_channel의 ref==0 체크.
	 * 값 범위: 0~정수.
	 * 동기화: 같은 io_channel을 가진 동일 스레드에서만 다뤄짐 → 무락. */

	/** Persistent Reservation Generation */
	uint32_t pr_generation;
	/* [한국어] PR generation 카운터(SPC-4 5.6.6) — registrant 변경 시마다 ++.
	 * 설정자: scsi_pr_register_registrant/unregister/preempt 등.
	 * 읽는 자: scsi_pr_in_read_keys / read_reservations / read_full_status (header에 BE32로).
	 * 값 범위: 32비트 wrap-around 가능.
	 * 동기화: lun->thread. */
	/** Registrant head for I_T nexus */
	TAILQ_HEAD(, spdk_scsi_pr_registrant) reg_head;
	/* [한국어] 이 LUN에 등록된 PR registrant 리스트.
	 * 설정자: scsi_pr_register_registrant(insert), unregister(remove).
	 * 읽는 자: scsi_pr_get_registrant 매칭, READ KEYS/FULL STATUS 응답.
	 * 값 범위: 0개 이상.
	 * 동기화: lun->thread. */
	/** Reservation for the LUN */
	struct spdk_scsi_pr_reservation reservation;
	/* [한국어] LUN의 현재 reservation 상태 — flags/holder/rtype/crkey.
	 * 설정자: scsi_pr_reserve_reservation/release_reservation, scsi2_reserve/release.
	 * 읽는 자: scsi_pr_check (R/W 허용 결정), READ RESERVATIONS, scsi2_reserve_check.
	 * 값 범위: 위 구조체 정의.
	 * 동기화: lun->thread. */
	/** Reservation holder for SPC2 RESERVE(6) and RESERVE(10) */
	struct spdk_scsi_pr_registrant scsi2_holder;
	/* [한국어] SPC-2 RESERVE 모드에서의 holder 정보(임시 등록자 형태로 보관).
	 *          SPC-2와 PR이 같은 reservation 슬롯을 공유하므로 별도 등록자 객체로 표현한다.
	 * 설정자: scsi2_reserve.
	 * 읽는 자: scsi2_release(zero out), scsi2_reserve_check(holder 매칭).
	 * 값 범위: spdk_scsi_pr_registrant 형식.
	 * 동기화: lun->thread. */
};

/* [한국어] LUN 생성/소멸 — lun.c 구현. dev.c가 호출. */
struct spdk_scsi_lun *scsi_lun_construct(const char *bdev_name,
		void (*resize_cb)(const struct spdk_scsi_lun *, void *),
		void *resize_ctx,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx);
void scsi_lun_destruct(struct spdk_scsi_lun *lun);
/* [한국어] LUN 단위 task 실행기/완료기 — IO와 mgmt(reset 등) 두 종류로 분리 운용.
 *          외부에서 spdk_scsi_dev_queue_task → scsi_lun_execute_task로 위임. */
void scsi_lun_execute_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
void scsi_lun_execute_mgmt_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
bool scsi_lun_has_pending_mgmt_tasks(const struct spdk_scsi_lun *lun,
				     const struct spdk_scsi_port *initiator_port);
/* [한국어] task 완료 보고 — bdev I/O 콜백 또는 동기 완료 경로에서 호출.
 *          tasks 리스트에서 제거 후 task->cpl_fn 호출. */
void scsi_lun_complete_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
/* [한국어] reset task 완료 — outstanding IO가 끝날 때까지 reset_poller로 대기 후 cpl. */
void scsi_lun_complete_reset_task(struct spdk_scsi_lun *lun, struct spdk_scsi_task *task);
/* [한국어] 특정 initiator의 pending(또는 모든 initiator) task 존재 여부 — 프론트엔드의 abort/cleanup 가드용. */
bool scsi_lun_has_pending_tasks(const struct spdk_scsi_lun *lun,
				const struct spdk_scsi_port *initiator_port);
/* [한국어] LUN io_channel 획득/해제 — ref count 기반. */
int scsi_lun_allocate_io_channel(struct spdk_scsi_lun *lun);
void scsi_lun_free_io_channel(struct spdk_scsi_lun *lun);

/* [한국어] dev.c의 g_devs[] 시작 포인터 — 정적 배열이므로 lock 불필요. RPC 등이 사용. */
struct spdk_scsi_dev *scsi_dev_get_list(void);

/* [한국어] port.c 내부 구축자/소멸자 — 동적 할당이 아니라 기존 구조체 슬롯에 대해 동작. */
int scsi_port_construct(struct spdk_scsi_port *port, uint64_t id,
			uint16_t index, const char *name);
void scsi_port_destruct(struct spdk_scsi_port *port);

/* [한국어] SCSI CDB → bdev I/O 변환 / LUN reset → bdev reset. scsi_bdev.c에서 구현. */
int bdev_scsi_execute(struct spdk_scsi_task *task);
void bdev_scsi_reset(struct spdk_scsi_task *task);

/* [한국어] DIF(데이터 무결성 필드) 컨텍스트 추출 — NVMe-oF/iSCSI가 PI 정보 필요할 때 호출. */
bool bdev_scsi_get_dif_ctx(struct spdk_bdev *bdev, struct spdk_scsi_task *task,
			   struct spdk_dif_ctx *dif_ctx);

/* [한국어] SPC-3 Persistent Reservation 처리(scsi_pr.c).
 *          scsi_pr_out: PERSISTENT RESERVE OUT 명령(REGISTER/RESERVE/RELEASE/CLEAR/PREEMPT).
 *          scsi_pr_in : PERSISTENT RESERVE IN  명령(READ KEYS/RESERVATION/REPORT_CAP/FULL_STATUS).
 *          scsi_pr_check: 일반 R/W 명령 진입 시 PR 충돌 검사 → 0 허용/-1 reservation conflict. */
int scsi_pr_out(struct spdk_scsi_task *task, uint8_t *cdb, uint8_t *data, uint16_t data_len);
int scsi_pr_in(struct spdk_scsi_task *task, uint8_t *cdb, uint8_t *data, uint16_t data_len);
int scsi_pr_check(struct spdk_scsi_task *task);

/* [한국어] SPC-2 RESERVE/RELEASE 처리. PR이 활성화된 LUN에서는 SPC-2가 호환 모드로 동작.
 *          scsi2_reserve_check: 일반 R/W 명령 진입 시 SPC-2 reservation 충돌 검사. */
int scsi2_reserve(struct spdk_scsi_task *task, uint8_t *cdb);
int scsi2_release(struct spdk_scsi_task *task);
int scsi2_reserve_check(struct spdk_scsi_task *task);

#endif /* SPDK_SCSI_INTERNAL_H */
/* [한국어] include guard 종료. */
