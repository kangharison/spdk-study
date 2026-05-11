/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI 서브시스템 초기화·종료 및 SPDK 프레임워크 통합 (iscsi_subsystem.c)
 *
 * === 파일의 역할 ===
 * iSCSI 타깃 라이브러리 전체의 라이프사이클 진입점. 구체적으로 (1) 전역 옵션
 * (g_iscsi/g_spdk_iscsi_opts) 초기화·검증, (2) 메모리 풀(pdu_pool, session_pool, task_pool,
 * immediate_data_pool, data_out_pool) 생성·해제, (3) 각 CPU 코어에 SPDK thread를 만들어
 * iscsi_poll_group(연결 묶음) 등록, (4) 인증 그룹/CHAP 시크릿 관리, (5) authfile 파싱,
 * (6) JSON-RPC config 직렬화, (7) spdk_iscsi_init/spdk_iscsi_fini 콜백 핸드셰이크를 담당한다.
 * SPDK subsystem 등록 매크로(SPDK_LOG_REGISTER_COMPONENT)로 전체 SPDK 부팅 흐름에 합류한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 호출 체인:
 *   spdk_app_start → subsystem 등록 순회 → iscsi_subsystem_init → spdk_iscsi_init(cb)
 *     → iscsi_parse_globals() → iscsi_initialize_global_params/all_pools/initialize_iscsi_conns
 *     → initialize_iscsi_poll_group() (SPDK_ENV_FOREACH_CORE 코어별 thread 생성)
 *     → 각 코어 _iscsi_init_thread → spdk_get_io_channel(&g_iscsi) → poll_group_create
 *     → _iscsi_init_thread_done → refcnt 0 도달 시 iscsi_parse_configuration → cb_fn 호출.
 * 데이터 평면(I/O):
 *   각 conn은 conn->pg(spdk_iscsi_poll_group)에 STAILQ_INSERT되며 iscsi_poll_group_poll()이
 *   group->sock_group을 polling → conn별 PDU 처리 → bdev I/O 발행.
 * 셧다운 호출 체인:
 *   spdk_iscsi_fini(cb) → iscsi_portal_grp_close_all → shutdown_iscsi_conns →
 *   shutdown_iscsi_conns_done → spdk_for_each_channel(_iscsi_fini_thread) →
 *   _iscsi_fini_dev_unreg → spdk_io_device_unregister → iscsi_fini_done → cb_fn.
 * 실행 컨텍스트: g_init_thread는 보통 SPDK app의 메인 thread이며, 각 poll_group은 자신의
 * SPDK thread/CPU 코어에 고정.
 *
 * === 타 모듈과의 연결 ===
 * - iscsi.h: g_iscsi 전역, 모든 도메인 상수.
 * - init_grp.h/portal_grp.h/tgt_node.h: fini 시 일괄 해제 호출.
 * - conn.h: initialize_iscsi_conns(), shutdown_iscsi_conns(), iscsi_conn_destruct(),
 *   iscsi_conn_handle_nop().
 * - task.h: spdk_iscsi_task 구조 (task_pool 항목 크기).
 * - thread.h (SPDK): spdk_thread_create, spdk_io_device_register, spdk_for_each_channel.
 * - sock.h (SPDK): spdk_sock_group_create/poll/close — poll_group의 sock_group 백엔드.
 * - env.h (SPDK): spdk_mempool_create_ctor, SPDK_ENV_FOREACH_CORE — DPDK 풀/스레드.
 * - 데이터 흐름: opts → g_iscsi 전역 → mempool 객체 → conn/sess 자료구조 → 네트워크 I/O.
 *
 * === 주요 함수/구조체 요약 ===
 * - mobj_ctor: pool 항목 초기화 — spdk_mobj 헤더 + 4KB 정렬 데이터 buffer.
 * - iscsi_initialize_pdu_pool/_task_pool/_session_pool: 5종 mempool 생성.
 * - iscsi_get_pdu/iscsi_put_pdu: PDU 라이프사이클 (ref count + mobj 반환).
 * - iscsi_opts_alloc/init/copy/free/verify: 사용자 옵션 ADT.
 * - iscsi_set_global_params: opts → g_iscsi 적용.
 * - iscsi_add_auth_group/iscsi_auth_group_add_secret: CHAP 인증 DB.
 * - iscsi_chap_get_authinfo: 인증 검사 시 user→secret 조회.
 * - iscsi_poll_group_create/destroy/poll/handle_nop: poll group SPDK io_device 콜백.
 * - initialize_iscsi_poll_group: 코어별 SPDK thread 생성 + poll_group io_channel 획득.
 * - spdk_iscsi_init/spdk_iscsi_fini: 라이브러리 외부 진입점.
 * - spdk_iscsi_config_json: save_config 시 모든 iSCSI 상태 직렬화.
 * - struct spdk_iscsi_poll_group: SPDK io_channel context — connections STAILQ + sock_group +
 *   poller(iscsi_poll_group_poll, 0us=매 tick) + nop_poller(1초).
 */

#include "spdk/string.h"
/* [한국어] spdk_strerror, strdup 등. */
#include "spdk/likely.h"
/* [한국어] spdk_unlikely() — 분기 예측 힌트 매크로. */

#include "iscsi/iscsi.h"
/* [한국어] g_iscsi 전역, ISCSI_*, DEFAULT_*, struct spdk_iscsi_globals/sess/pdu. */
#include "iscsi/init_grp.h"
/* [한국어] iscsi_init_grps_destroy, IG 직렬화. */
#include "iscsi/portal_grp.h"
/* [한국어] iscsi_portal_grp_close_all/destroy, PG 직렬화. */
#include "iscsi/conn.h"
/* [한국어] initialize_iscsi_conns/shutdown_iscsi_conns/iscsi_conn_destruct/iscsi_conn_handle_nop. */
#include "iscsi/task.h"
/* [한국어] struct spdk_iscsi_task — task_pool 객체 크기. */
#include "iscsi/tgt_node.h"
/* [한국어] iscsi_shutdown_tgt_nodes, tgt_node 직렬화. */

#include "spdk/log.h"
/* [한국어] SPDK_*LOG. */

/*
 * [한국어]
 * g_spdk_iscsi_opts - 전역 옵션 임시 보관 포인터.
 *
 * spdk_iscsi_init() 호출 전에 RPC iscsi_set_options 등으로 미리 채워둘 수 있다.
 * iscsi_initialize_global_params에서 g_iscsi에 복사 후 free되어 NULL이 된다.
 * 설정자: RPC iscsi_set_options 또는 iscsi_opts_alloc(기본값).
 * 읽는 자: iscsi_initialize_global_params (단 1회).
 * 동기화: 초기화 단일 thread에서만 접근하므로 별도 락 없음.
 */
struct spdk_iscsi_opts *g_spdk_iscsi_opts = NULL;

/*
 * [한국어]
 * g_init_thread - spdk_iscsi_init 호출 시 활성 SPDK thread.
 *
 * 모든 poll_group 생성·완료 메시지를 이 thread로 보내 직렬 결과 처리. 또한 fini 단계에서
 * io_device_unregister 필요 여부 판정에 사용 (NULL이면 init이 부분만 진행됨).
 */
static struct spdk_thread *g_init_thread = NULL;
/*
 * [한국어]
 * g_init_cb_fn / g_init_cb_arg - spdk_iscsi_init 완료 후 호출할 콜백/인자.
 *
 * iscsi_init_complete()에서 발사 후 NULL 리셋.
 */
static spdk_iscsi_init_cb g_init_cb_fn = NULL;
static void *g_init_cb_arg = NULL;

/*
 * [한국어]
 * g_fini_cb_fn / g_fini_cb_arg - spdk_iscsi_fini 완료 후 호출할 콜백.
 *
 * iscsi_fini_done()에서 발사. 종료 흐름 끝까지 살아있어야 하므로 NULL 리셋 안 함.
 */
static spdk_iscsi_fini_cb g_fini_cb_fn;
static void *g_fini_cb_arg;

#define ISCSI_DATA_BUFFER_ALIGNMENT	(0x1000)
/* [한국어] PDU data buffer 정렬 (4KB). DMA/페이지 정렬을 만족시켜 zero-copy 경로에서 효율적. */
#define ISCSI_DATA_BUFFER_MASK		(ISCSI_DATA_BUFFER_ALIGNMENT - 1)
/* [한국어] 4KB 마스크 — (ptr + ALIGN) & ~MASK 로 정렬 올림. */

/*
 * [한국어]
 * mobj_ctor - mempool 객체 생성 콜백 (spdk_mempool_create_ctor 등록).
 *
 * @mp: 소속 mempool.
 * @arg: 무시 (ctor 등록 시 NULL).
 * @_m: 새로 생성된 객체 메모리 시작 주소.
 * @i: 객체 인덱스.
 *
 * spdk_mobj 헤더 뒤에 ISCSI_DATA_BUFFER_ALIGNMENT(4KB)로 정렬된 데이터 버퍼가 위치하도록
 * m->buf를 계산. 헤더와 buffer는 한 chunk 내 인접하지만 buffer는 정렬 올림된 위치에서 시작.
 *
 * 호출 체인:
 *   spdk_mempool_create_ctor → DPDK rte_mempool obj_init → [mobj_ctor]
 */
static void
mobj_ctor(struct spdk_mempool *mp, __attribute__((unused)) void *arg,
	  void *_m, __attribute__((unused)) unsigned i)
{
	struct spdk_mobj *m = _m;
	/* [한국어] mempool 객체를 mobj 헤더로 캐스팅. */

	m->mp = mp;
	/* [한국어] 객체가 어느 풀에서 왔는지 기록 — put 시 풀 식별. */
	m->buf = (uint8_t *)m + sizeof(struct spdk_mobj);
	/* [한국어] 헤더 다음 위치를 buffer 후보로. */
	m->buf = (void *)((uintptr_t)((uint8_t *)m->buf + ISCSI_DATA_BUFFER_ALIGNMENT) &
			  ~ISCSI_DATA_BUFFER_MASK);
	/* [한국어] 4KB 정렬 올림 — DPDK 페이지 정렬 경계로 맞춰 DMA·캐시 친화. */
}

/*
 * [한국어]
 * iscsi_initialize_pdu_pool - PDU/Immediate Data/Data Out 3종 mempool 생성.
 *
 * @return: 0 성공, -1 실패 (이미 존재 또는 ENOMEM).
 *
 * 풀 크기는 g_iscsi.{pdu,immediate_data,data_out}_pool_size에서 가져온다 (PDU_POOL_SIZE 등).
 * Immediate/Data Out 풀은 PDU 수신 buffer로 사용하므로 mobj_ctor로 정렬된 buffer를 생성.
 *
 * 호출 체인:
 *   iscsi_initialize_all_pools → [iscsi_initialize_pdu_pool]
 */
static int
iscsi_initialize_pdu_pool(void)
{
	struct spdk_iscsi_globals *iscsi = &g_iscsi;
	/* [한국어] 전역 alias. */
	int imm_mobj_size = SPDK_BDEV_BUF_SIZE_WITH_MD(iscsi_get_max_immediate_data_size()) +
			    sizeof(struct spdk_mobj) + ISCSI_DATA_BUFFER_ALIGNMENT;
	/* [한국어] Immediate Data 객체 크기 — 헤더 + (정렬 마진을 위한) 4KB 추가 + bdev 메타데이터 포함. */
	int dout_mobj_size = SPDK_BDEV_BUF_SIZE_WITH_MD(SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH) +
			     sizeof(struct spdk_mobj) + ISCSI_DATA_BUFFER_ALIGNMENT;
	/* [한국어] Data Out 객체 크기 — Max Recv Data Segment Length 기준. */

	/* create PDU pool */
	iscsi->pdu_pool = spdk_mempool_create("PDU_Pool",
					      iscsi->pdu_pool_size,
					      sizeof(struct spdk_iscsi_pdu),
					      256, SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] PDU 자체의 mempool — header만, data buffer는 별도 mobj 풀.
	 * 256: per-core cache size (DPDK rte_mempool 기능 — 코어별 캐시로 락 회피).
	 * SPDK_ENV_NUMA_ID_ANY: 어떤 NUMA 노드든 허용. */
	if (!iscsi->pdu_pool) {
		if (spdk_mempool_lookup("PDU_Pool") != NULL) {
			/* [한국어] 이미 존재 — 두 번째 프로세스가 같은 hugepage를 attach한 상태. */
			SPDK_ERRLOG("Cannot create PDU pool: already exists\n");
			SPDK_ERRLOG("Probably running in multiprocess environment, which is "
				    "unsupported by the iSCSI library\n");
		} else {
			SPDK_ERRLOG("create PDU pool failed\n");
		}
		return -1;
	}

	iscsi->pdu_immediate_data_pool = spdk_mempool_create_ctor("PDU_immediate_data_Pool",
					 iscsi->immediate_data_pool_size,
					 imm_mobj_size, 256,
					 SPDK_ENV_NUMA_ID_ANY,
					 mobj_ctor, NULL);
	/* [한국어] Immediate Data 풀 — ctor=mobj_ctor 호출하여 정렬된 buf 셋업. */
	if (!iscsi->pdu_immediate_data_pool) {
		SPDK_ERRLOG("create PDU immediate data pool failed\n");
		return -1;
	}

	iscsi->pdu_data_out_pool = spdk_mempool_create_ctor("PDU_data_out_Pool",
				   iscsi->data_out_pool_size,
				   dout_mobj_size, 256,
				   SPDK_ENV_NUMA_ID_ANY,
				   mobj_ctor, NULL);
	/* [한국어] Data Out 풀 — initiator 측 R2T 응답 등 큰 데이터 segment 수신용. */
	if (!iscsi->pdu_data_out_pool) {
		SPDK_ERRLOG("create PDU data out pool failed\n");
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_sess_ctor - 세션 mempool 객체 생성 콜백.
 *
 * @arg: spdk_iscsi_globals* (create_ctor 인자로 전달).
 * @session_buf: 새 객체.
 * @index: 객체 인덱스 (0..MaxSessions-1).
 *
 * 세션 인덱스를 g_iscsi.session[] 배열에 등록하고 tsih(Target Session Identifier Handle)를
 * index+1로 설정. tsih=0은 RFC 3720에 의해 예약(미할당) 의미로 사용 금지.
 */
static void
iscsi_sess_ctor(struct spdk_mempool *pool, void *arg, void *session_buf,
		unsigned index)
{
	struct spdk_iscsi_globals		*iscsi = arg;
	/* [한국어] arg 캐스팅. */
	struct spdk_iscsi_sess	*sess = session_buf;
	/* [한국어] 객체 캐스팅. */

	iscsi->session[index] = sess;
	/* [한국어] index→sess 역참조 가능 테이블 — RFC 3720 tsih 발급 시 사용. */

	/* tsih 0 is reserved, so start tsih values at 1. */
	sess->tsih = index + 1;
	/* [한국어] tsih=0 예약 → 인덱스 0의 객체에 tsih=1 부여. RFC 3720 §10.12. */
}

#define DEFAULT_TASK_POOL_SIZE 32768
/* [한국어] SCSI task pool 기본 크기 (32K 동시 task) — 한 노드에서의 outstanding 한도. */

/*
 * [한국어]
 * iscsi_initialize_task_pool - SCSI task 객체 mempool 생성.
 *
 * SPDK iscsi task = SCSI command + iSCSI 트랜스포트 상태. 핫패스 객체이므로 코어별 캐시 128.
 */
static int
iscsi_initialize_task_pool(void)
{
	struct spdk_iscsi_globals *iscsi = &g_iscsi;
	/* [한국어] 전역 alias. */

	/* create scsi_task pool */
	iscsi->task_pool = spdk_mempool_create("SCSI_TASK_Pool",
					       DEFAULT_TASK_POOL_SIZE,
					       sizeof(struct spdk_iscsi_task),
					       128, SPDK_ENV_NUMA_ID_ANY);
	/* [한국어] task 풀. cache=128 (PDU보다 작게 — 객체가 더 크기 때문). */
	if (!iscsi->task_pool) {
		SPDK_ERRLOG("create task pool failed\n");
		return -1;
	}

	return 0;
}

#define SESSION_POOL_SIZE(iscsi)	(iscsi->MaxSessions)
/* [한국어] 세션 풀 크기 = 옵션의 MaxSessions. */

/*
 * [한국어]
 * iscsi_initialize_session_pool - iSCSI 세션 mempool 생성.
 *
 * ctor=iscsi_sess_ctor — 객체 생성 시 g_iscsi.session[] 등록 + tsih 부여.
 */
static int
iscsi_initialize_session_pool(void)
{
	struct spdk_iscsi_globals *iscsi = &g_iscsi;
	/* [한국어] 전역 alias. */

	iscsi->session_pool = spdk_mempool_create_ctor("Session_Pool",
			      SESSION_POOL_SIZE(iscsi),
			      sizeof(struct spdk_iscsi_sess), 0,
			      SPDK_ENV_NUMA_ID_ANY,
			      iscsi_sess_ctor, iscsi);
	/* [한국어] 세션 풀. cache=0 (세션 라이프사이클이 길어 캐시 불필요), arg=&g_iscsi 전달. */
	if (!iscsi->session_pool) {
		SPDK_ERRLOG("create session pool failed\n");
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_initialize_all_pools - 위 3종 mempool 일괄 생성.
 *
 * 어느 하나라도 실패하면 -1; 호출자가 전체 정리 책임 (free_pools).
 */
static int
iscsi_initialize_all_pools(void)
{
	if (iscsi_initialize_pdu_pool() != 0) {
		return -1;
	}

	if (iscsi_initialize_session_pool() != 0) {
		return -1;
	}

	if (iscsi_initialize_task_pool() != 0) {
		return -1;
	}

	return 0;
	/* [한국어] 모두 성공. */
}

/*
 * [한국어]
 * iscsi_check_pool - 풀 객체 수가 기대값과 일치하는지 검증 (leak 진단).
 *
 * fini 단계에서 호출 — 누수가 있으면 ERRLOG. 운영에는 영향 없으나 개발 시 유용.
 */
static void
iscsi_check_pool(struct spdk_mempool *pool, size_t count)
{
	if (pool && spdk_mempool_count(pool) != count) {
		SPDK_ERRLOG("spdk_mempool_count(%s) == %zu, should be %zu\n",
			    spdk_mempool_get_name(pool), spdk_mempool_count(pool), count);
		/* [한국어] 사용 중인 객체가 있다는 의미 — leak 또는 outstanding I/O. */
	}
}

/*
 * [한국어]
 * iscsi_check_pools - 5종 풀 모두에 대해 leak 검사.
 */
static void
iscsi_check_pools(void)
{
	struct spdk_iscsi_globals *iscsi = &g_iscsi;
	/* [한국어] 전역 alias. */

	iscsi_check_pool(iscsi->pdu_pool, iscsi->pdu_pool_size);
	iscsi_check_pool(iscsi->session_pool, SESSION_POOL_SIZE(iscsi));
	iscsi_check_pool(iscsi->pdu_immediate_data_pool, iscsi->immediate_data_pool_size);
	iscsi_check_pool(iscsi->pdu_data_out_pool, iscsi->data_out_pool_size);
	iscsi_check_pool(iscsi->task_pool, DEFAULT_TASK_POOL_SIZE);
}

/*
 * [한국어]
 * iscsi_free_pools - 5종 풀 free.
 *
 * spdk_mempool_free는 DPDK rte_mempool_free를 wrapping — hugepage 메모리 회수.
 */
static void
iscsi_free_pools(void)
{
	struct spdk_iscsi_globals *iscsi = &g_iscsi;
	/* [한국어] 전역 alias. */

	spdk_mempool_free(iscsi->pdu_pool);
	spdk_mempool_free(iscsi->session_pool);
	spdk_mempool_free(iscsi->pdu_immediate_data_pool);
	spdk_mempool_free(iscsi->pdu_data_out_pool);
	spdk_mempool_free(iscsi->task_pool);
}

/*
 * [한국어]
 * iscsi_put_pdu - PDU의 ref count를 1 감소시키고, 0 도달 시 자원 회수.
 *
 * @pdu: 대상 PDU (NULL 허용).
 *
 * mobj[0/1]은 immediate_data/data_out 풀의 데이터 buffer (zero-copy 경로). pdu->data가
 * 별도 malloc된 경우(mempool 외)는 직접 free. 마지막에 pdu_pool에 반환.
 *
 * 호출 체인:
 *   각 PDU 처리 완료 → [iscsi_put_pdu] → mobj 풀 반환 → spdk_mempool_put
 */
void
iscsi_put_pdu(struct spdk_iscsi_pdu *pdu)
{
	if (!pdu) {
		/* [한국어] NULL 허용 — caller 편의. */
		return;
	}

	assert(pdu->ref > 0);
	/* [한국어] 음수 ref 방지 — invariant. */
	pdu->ref--;
	/* [한국어] 단일 thread (conn의 SPDK thread)에서만 호출되므로 atomic 불필요. */

	if (pdu->ref == 0) {
		/* [한국어] 마지막 참조 해제 — 자원 회수. */
		if (pdu->mobj[0]) {
			iscsi_datapool_put(pdu->mobj[0]);
			/* [한국어] mobj[0] 데이터 buffer를 원래 풀로 반환 (mobj->mp 사용). */
		}
		if (pdu->mobj[1]) {
			iscsi_datapool_put(pdu->mobj[1]);
			/* [한국어] mobj[1] (split된 경우 두 번째 buffer). */
		}

		if (pdu->data && !pdu->data_from_mempool) {
			/* [한국어] mempool에서 온 게 아닌 경우만 직접 free. */
			free(pdu->data);
		}

		spdk_mempool_put(g_iscsi.pdu_pool, (void *)pdu);
		/* [한국어] PDU 자체를 풀에 반환 — DPDK ring으로 푸시. */
	}
}

/*
 * [한국어]
 * iscsi_get_pdu - PDU mempool에서 1개 객체 획득 후 초기화.
 *
 * @conn: 소유 연결.
 * @return: 새 PDU (실패 시 abort — 풀 부족은 치명적 설계 가정).
 *
 * AHS/sense 영역은 보존(memset 범위에서 제외) — 이전 사용자가 남긴 buffer를 재사용 가능.
 * ref=1, conn 백포인터 설정, CRC32C 초기값 셋팅.
 *
 * 호출 체인:
 *   PDU 생성 경로 (iscsi_read_pdu, write 응답 등) → [iscsi_get_pdu]
 */
struct spdk_iscsi_pdu *iscsi_get_pdu(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *pdu;
	/* [한국어] 새 PDU 객체. */

	assert(conn != NULL);
	/* [한국어] invariant. */
	pdu = spdk_mempool_get(g_iscsi.pdu_pool);
	/* [한국어] 풀에서 객체 획득 — DPDK ring pop. */
	if (!pdu) {
		/* [한국어] 풀 고갈 — 풀 크기 산정 실수이므로 abort. */
		SPDK_ERRLOG("Unable to get PDU\n");
		abort();
	}

	/* we do not want to zero out the last part of the structure reserved for AHS and sense data */
	memset(pdu, 0, offsetof(struct spdk_iscsi_pdu, ahs));
	/* [한국어] ahs 필드 직전까지만 0 초기화 — ahs/sense는 별도 setup. */
	pdu->ref = 1;
	/* [한국어] 신규 ref = 1 (caller 소유). */
	pdu->conn = conn;
	/* [한국어] 백포인터 — completion 시 conn 식별. */
	/* Initialize CRC. */
	pdu->crc32c = SPDK_CRC32C_INITIAL;
	/* [한국어] CRC32C 누적 시작값 (RFC 3720 HeaderDigest/DataDigest). */

	return pdu;
}

/*
 * [한국어]
 * iscsi_log_globals - g_iscsi의 핵심 옵션을 디버그 로그로 출력 (init 트레이스).
 *
 * RPC iscsi_set_options나 기본값 적용이 의도대로 되었는지 확인 용도.
 */
static void
iscsi_log_globals(void)
{
	SPDK_DEBUGLOG(iscsi, "AuthFile %s\n",
		      g_iscsi.authfile ? g_iscsi.authfile : "(none)");
	/* [한국어] CHAP secret 파일 경로 (설정 시). */
	SPDK_DEBUGLOG(iscsi, "NodeBase %s\n", g_iscsi.nodebase);
	/* [한국어] IQN base ("iqn.2016-06.io.spdk" 등). */
	SPDK_DEBUGLOG(iscsi, "MaxSessions %d\n", g_iscsi.MaxSessions);
	SPDK_DEBUGLOG(iscsi, "MaxConnectionsPerSession %d\n",
		      g_iscsi.MaxConnectionsPerSession);
	SPDK_DEBUGLOG(iscsi, "MaxQueueDepth %d\n", g_iscsi.MaxQueueDepth);
	SPDK_DEBUGLOG(iscsi, "DefaultTime2Wait %d\n",
		      g_iscsi.DefaultTime2Wait);
	SPDK_DEBUGLOG(iscsi, "DefaultTime2Retain %d\n",
		      g_iscsi.DefaultTime2Retain);
	SPDK_DEBUGLOG(iscsi, "FirstBurstLength %d\n",
		      g_iscsi.FirstBurstLength);
	SPDK_DEBUGLOG(iscsi, "ImmediateData %s\n",
		      g_iscsi.ImmediateData ? "Yes" : "No");
	SPDK_DEBUGLOG(iscsi, "AllowDuplicateIsid %s\n",
		      g_iscsi.AllowDuplicateIsid ? "Yes" : "No");
	SPDK_DEBUGLOG(iscsi, "ErrorRecoveryLevel %d\n",
		      g_iscsi.ErrorRecoveryLevel);
	SPDK_DEBUGLOG(iscsi, "Timeout %d\n", g_iscsi.timeout);
	SPDK_DEBUGLOG(iscsi, "NopInInterval %d\n",
		      g_iscsi.nopininterval);
	if (g_iscsi.disable_chap) {
		/* [한국어] CHAP 비활성. */
		SPDK_DEBUGLOG(iscsi,
			      "DiscoveryAuthMethod None\n");
	} else if (!g_iscsi.require_chap) {
		/* [한국어] CHAP 선택적 (Auto). */
		SPDK_DEBUGLOG(iscsi,
			      "DiscoveryAuthMethod Auto\n");
	} else {
		/* [한국어] CHAP 강제. */
		SPDK_DEBUGLOG(iscsi,
			      "DiscoveryAuthMethod %s %s\n",
			      g_iscsi.require_chap ? "CHAP" : "",
			      g_iscsi.mutual_chap ? "Mutual" : "");
	}

	if (g_iscsi.chap_group == 0) {
		/* [한국어] 기본 인증 그룹 미지정. */
		SPDK_DEBUGLOG(iscsi,
			      "DiscoveryAuthGroup None\n");
	} else {
		SPDK_DEBUGLOG(iscsi,
			      "DiscoveryAuthGroup AuthGroup%d\n",
			      g_iscsi.chap_group);
	}

	SPDK_DEBUGLOG(iscsi, "MaxLargeDataInPerConnection %d\n",
		      g_iscsi.MaxLargeDataInPerConnection);

	SPDK_DEBUGLOG(iscsi, "MaxR2TPerConnection %d\n",
		      g_iscsi.MaxR2TPerConnection);
}

/*
 * [한국어]
 * 풀 크기 자동 산정 매크로.
 *
 * NUM_PDU_PER_CONNECTION: 한 conn이 동시에 가질 수 있는 PDU 수 추정.
 *   2*MaxQueueDepth (송수신) + MaxLargeDataInPerConnection + 2*MaxR2TPerConnection (R2T 송수신)
 *   + 8 (제어 PDU 여유분).
 * PDU_POOL_SIZE = MaxSessions * NUM_PDU_PER_CONNECTION.
 * IMMEDIATE_DATA_POOL_SIZE = MaxSessions * 128 (세션당 immediate 버퍼 여유분).
 * DATA_OUT_POOL_SIZE = MaxSessions * MAX_DATA_OUT_PER_CONNECTION.
 */
#define NUM_PDU_PER_CONNECTION(opts)	(2 * (opts->MaxQueueDepth +	\
					 opts->MaxLargeDataInPerConnection +	\
					 2 * opts->MaxR2TPerConnection + 8))
#define PDU_POOL_SIZE(opts)		(opts->MaxSessions * NUM_PDU_PER_CONNECTION(opts))
#define IMMEDIATE_DATA_POOL_SIZE(opts)	(opts->MaxSessions * 128)
#define DATA_OUT_POOL_SIZE(opts)	(opts->MaxSessions * MAX_DATA_OUT_PER_CONNECTION)

/*
 * [한국어]
 * iscsi_opts_init - opts 구조체를 모든 항목 기본값으로 채움.
 *
 * @opts: 미초기화 또는 calloc된 buffer.
 *
 * iscsi_opts_alloc()이 calloc 후 호출. RPC iscsi_set_options에서 partial 변경 시 미지정
 * 필드는 이 기본값을 가진다.
 */
static void
iscsi_opts_init(struct spdk_iscsi_opts *opts)
{
	opts->MaxSessions = DEFAULT_MAX_SESSIONS;
	/* [한국어] 기본 128. */
	opts->MaxConnectionsPerSession = DEFAULT_MAX_CONNECTIONS_PER_SESSION;
	/* [한국어] 기본 2 (RFC 3720 §11.2 권장 1+). */
	opts->MaxQueueDepth = DEFAULT_MAX_QUEUE_DEPTH;
	/* [한국어] 기본 64. */
	opts->DefaultTime2Wait = DEFAULT_DEFAULTTIME2WAIT;
	opts->DefaultTime2Retain = DEFAULT_DEFAULTTIME2RETAIN;
	opts->FirstBurstLength = SPDK_ISCSI_FIRST_BURST_LENGTH;
	/* [한국어] 64KB. */
	opts->ImmediateData = DEFAULT_IMMEDIATEDATA;
	opts->AllowDuplicateIsid = false;
	/* [한국어] 동일 ISID 중복 허용 안 함 (RFC 3720 §10.12.2). */
	opts->ErrorRecoveryLevel = DEFAULT_ERRORRECOVERYLEVEL;
	opts->timeout = DEFAULT_TIMEOUT;
	opts->nopininterval = DEFAULT_NOPININTERVAL;
	opts->disable_chap = false;
	opts->require_chap = false;
	opts->mutual_chap = false;
	opts->chap_group = 0;
	opts->authfile = NULL;
	opts->nodebase = NULL;
	opts->MaxLargeDataInPerConnection = DEFAULT_MAX_LARGE_DATAIN_PER_CONNECTION;
	opts->MaxR2TPerConnection = DEFAULT_MAXR2T;
	opts->pdu_pool_size = PDU_POOL_SIZE(opts);
	opts->immediate_data_pool_size = IMMEDIATE_DATA_POOL_SIZE(opts);
	opts->data_out_pool_size = DATA_OUT_POOL_SIZE(opts);
	/* [한국어] 풀 크기 자동 산정. */
}

/*
 * [한국어]
 * iscsi_opts_alloc - opts 객체 calloc + 기본값 초기화.
 *
 * @return: 새 opts 또는 NULL.
 */
struct spdk_iscsi_opts *
iscsi_opts_alloc(void)
{
	struct spdk_iscsi_opts *opts;
	/* [한국어] 새 객체. */

	opts = calloc(1, sizeof(*opts));
	if (!opts) {
		SPDK_ERRLOG("calloc() failed for iscsi options\n");
		return NULL;
	}

	iscsi_opts_init(opts);
	/* [한국어] 기본값 채움. */

	return opts;
}

/*
 * [한국어]
 * iscsi_opts_free - opts 객체와 strdup된 문자열 free.
 */
void
iscsi_opts_free(struct spdk_iscsi_opts *opts)
{
	free(opts->authfile);
	/* [한국어] strdup된 경우 free; NULL이면 free(NULL)은 no-op. */
	free(opts->nodebase);
	free(opts);
}

/* Deep copy of spdk_iscsi_opts */
/*
 * [한국어]
 * iscsi_opts_copy - opts 깊은 복사.
 *
 * authfile/nodebase는 strdup으로 별도 복사. 어느 한 strdup이라도 실패하면 부분 free 후
 * NULL 반환 (오류 일관성).
 */
struct spdk_iscsi_opts *
iscsi_opts_copy(struct spdk_iscsi_opts *src)
{
	struct spdk_iscsi_opts *dst;
	/* [한국어] 새 객체. */

	dst = calloc(1, sizeof(*dst));
	if (!dst) {
		SPDK_ERRLOG("calloc() failed for iscsi options\n");
		return NULL;
	}

	if (src->authfile) {
		/* [한국어] authfile 깊은 복사. */
		dst->authfile = strdup(src->authfile);
		if (!dst->authfile) {
			free(dst);
			SPDK_ERRLOG("failed to strdup for auth file %s\n", src->authfile);
			return NULL;
		}
	}

	if (src->nodebase) {
		/* [한국어] nodebase 깊은 복사. */
		dst->nodebase = strdup(src->nodebase);
		if (!dst->nodebase) {
			free(dst->authfile);
			/* [한국어] 부분 정리. */
			free(dst);
			SPDK_ERRLOG("failed to strdup for nodebase %s\n", src->nodebase);
			return NULL;
		}
	}

	dst->MaxSessions = src->MaxSessions;
	dst->MaxConnectionsPerSession = src->MaxConnectionsPerSession;
	dst->MaxQueueDepth = src->MaxQueueDepth;
	dst->DefaultTime2Wait = src->DefaultTime2Wait;
	dst->DefaultTime2Retain = src->DefaultTime2Retain;
	dst->FirstBurstLength = src->FirstBurstLength;
	dst->ImmediateData = src->ImmediateData;
	dst->AllowDuplicateIsid = src->AllowDuplicateIsid;
	dst->ErrorRecoveryLevel = src->ErrorRecoveryLevel;
	dst->timeout = src->timeout;
	dst->nopininterval = src->nopininterval;
	dst->disable_chap = src->disable_chap;
	dst->require_chap = src->require_chap;
	dst->mutual_chap = src->mutual_chap;
	dst->chap_group = src->chap_group;
	dst->MaxLargeDataInPerConnection = src->MaxLargeDataInPerConnection;
	dst->MaxR2TPerConnection = src->MaxR2TPerConnection;
	dst->pdu_pool_size = src->pdu_pool_size;
	dst->immediate_data_pool_size = src->immediate_data_pool_size;
	dst->data_out_pool_size = src->data_out_pool_size;
	/* [한국어] 스칼라 일괄 복사. */

	return dst;
}

/*
 * [한국어]
 * iscsi_opts_verify - opts 값 범위 검증 + 기본 nodebase 보장.
 *
 * @return: 0 OK, -EINVAL/-ENOMEM.
 *
 * 모든 키에 대해 RFC 3720/SPDK 한도(MaxSessions ≤65535, MaxQueueDepth ≤256, …) 검사.
 * nodebase 미설정 시 SPDK_ISCSI_DEFAULT_NODEBASE("iqn.2016-06.io.spdk")로 채움.
 */
static int
iscsi_opts_verify(struct spdk_iscsi_opts *opts)
{
	if (!opts->nodebase) {
		/* [한국어] nodebase 누락 — 기본값으로 strdup. */
		opts->nodebase = strdup(SPDK_ISCSI_DEFAULT_NODEBASE);
		if (opts->nodebase == NULL) {
			SPDK_ERRLOG("strdup() failed for default nodebase\n");
			return -ENOMEM;
		}
	}

	if (opts->MaxSessions == 0 || opts->MaxSessions > 65535) {
		/* [한국어] 0 또는 16비트 초과 거부. */
		SPDK_ERRLOG("%d is invalid. MaxSessions must be more than 0 and no more than 65535\n",
			    opts->MaxSessions);
		return -EINVAL;
	}

	if (opts->MaxConnectionsPerSession == 0 || opts->MaxConnectionsPerSession > 65535) {
		SPDK_ERRLOG("%d is invalid. MaxConnectionsPerSession must be more than 0 and no more than 65535\n",
			    opts->MaxConnectionsPerSession);
		return -EINVAL;
	}

	if (opts->MaxQueueDepth == 0 || opts->MaxQueueDepth > 256) {
		/* [한국어] SPDK 한도 256. */
		SPDK_ERRLOG("%d is invalid. MaxQueueDepth must be more than 0 and no more than 256\n",
			    opts->MaxQueueDepth);
		return -EINVAL;
	}

	if (opts->DefaultTime2Wait > 3600) {
		/* [한국어] 1시간 초과 거부. */
		SPDK_ERRLOG("%d is invalid. DefaultTime2Wait must be no more than 3600\n",
			    opts->DefaultTime2Wait);
		return -EINVAL;
	}

	if (opts->DefaultTime2Retain > 3600) {
		SPDK_ERRLOG("%d is invalid. DefaultTime2Retain must be no more than 3600\n",
			    opts->DefaultTime2Retain);
		return -EINVAL;
	}

	if (opts->FirstBurstLength >= SPDK_ISCSI_MIN_FIRST_BURST_LENGTH) {
		/* [한국어] FBL 하한 통과 — MBL과의 일관성도 검사. */
		if (opts->FirstBurstLength > SPDK_ISCSI_MAX_BURST_LENGTH) {
			SPDK_ERRLOG("FirstBurstLength %d shall not exceed MaxBurstLength %d\n",
				    opts->FirstBurstLength, SPDK_ISCSI_MAX_BURST_LENGTH);
			return -EINVAL;
		}
	} else {
		SPDK_ERRLOG("FirstBurstLength %d shall be no less than %d\n",
			    opts->FirstBurstLength, SPDK_ISCSI_MIN_FIRST_BURST_LENGTH);
		return -EINVAL;
	}

	if (opts->ErrorRecoveryLevel > 2) {
		/* [한국어] RFC 3720 §6 ERL 0~2만 정의. */
		SPDK_ERRLOG("ErrorRecoveryLevel %d is not supported.\n", opts->ErrorRecoveryLevel);
		return -EINVAL;
	}

	if (opts->timeout < 0) {
		SPDK_ERRLOG("%d is invalid. timeout must not be less than 0\n", opts->timeout);
		return -EINVAL;
	}

	if (opts->nopininterval < 0 || opts->nopininterval > MAX_NOPININTERVAL) {
		SPDK_ERRLOG("%d is invalid. nopinterval must be between 0 and %d\n",
			    opts->nopininterval, MAX_NOPININTERVAL);
		return -EINVAL;
	}

	if (!iscsi_check_chap_params(opts->disable_chap, opts->require_chap,
				     opts->mutual_chap, opts->chap_group)) {
		/* [한국어] CHAP 4-튜플 모순 검사 (예: disable=true이면서 require=true 거부). */
		SPDK_ERRLOG("CHAP params in opts are illegal combination\n");
		return -EINVAL;
	}

	if (opts->MaxLargeDataInPerConnection == 0) {
		SPDK_ERRLOG("0 is invalid. MaxLargeDataInPerConnection must be more than 0\n");
		return -EINVAL;
	}

	if (opts->MaxR2TPerConnection == 0) {
		SPDK_ERRLOG("0 is invalid. MaxR2TPerConnection must be more than 0\n");
		return -EINVAL;
	}

	if (opts->pdu_pool_size == 0) {
		SPDK_ERRLOG("0 is invalid. pdu_pool_size must be more than 0\n");
		return -EINVAL;
	}

	if (opts->immediate_data_pool_size == 0) {
		SPDK_ERRLOG("0 is invalid. immediate_data_pool_size must be more than 0\n");
		return -EINVAL;
	}

	if (opts->data_out_pool_size == 0) {
		SPDK_ERRLOG("0 is invalid. data_out_pool_size must be more than 0\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_set_global_params - opts → g_iscsi 적용 (검증 포함).
 *
 * authfile/nodebase는 strdup으로 g_iscsi 소유 복사본을 만든다 (opts free 후에도 유효).
 * 모든 스칼라 필드는 단순 복사. 마지막에 iscsi_log_globals로 트레이스.
 */
static int
iscsi_set_global_params(struct spdk_iscsi_opts *opts)
{
	int rc;
	/* [한국어] verify 결과. */

	rc = iscsi_opts_verify(opts);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_iscsi_opts_verify() failed\n");
		return rc;
	}

	if (opts->authfile != NULL) {
		/* [한국어] authfile strdup → g_iscsi 소유. */
		g_iscsi.authfile = strdup(opts->authfile);
		if (!g_iscsi.authfile) {
			SPDK_ERRLOG("failed to strdup for auth file %s\n", opts->authfile);
			return -ENOMEM;
		}
	}

	g_iscsi.nodebase = strdup(opts->nodebase);
	/* [한국어] verify에서 NULL 보장 후 strdup. */
	if (!g_iscsi.nodebase) {
		SPDK_ERRLOG("failed to strdup for nodebase %s\n", opts->nodebase);
		return -ENOMEM;
	}

	g_iscsi.MaxSessions = opts->MaxSessions;
	g_iscsi.MaxConnectionsPerSession = opts->MaxConnectionsPerSession;
	g_iscsi.MaxQueueDepth = opts->MaxQueueDepth;
	g_iscsi.DefaultTime2Wait = opts->DefaultTime2Wait;
	g_iscsi.DefaultTime2Retain = opts->DefaultTime2Retain;
	g_iscsi.FirstBurstLength = opts->FirstBurstLength;
	g_iscsi.ImmediateData = opts->ImmediateData;
	g_iscsi.AllowDuplicateIsid = opts->AllowDuplicateIsid;
	g_iscsi.ErrorRecoveryLevel = opts->ErrorRecoveryLevel;
	g_iscsi.timeout = opts->timeout;
	g_iscsi.nopininterval = opts->nopininterval;
	g_iscsi.disable_chap = opts->disable_chap;
	g_iscsi.require_chap = opts->require_chap;
	g_iscsi.mutual_chap = opts->mutual_chap;
	g_iscsi.chap_group = opts->chap_group;
	g_iscsi.MaxLargeDataInPerConnection = opts->MaxLargeDataInPerConnection;
	g_iscsi.MaxR2TPerConnection = opts->MaxR2TPerConnection;
	g_iscsi.pdu_pool_size = opts->pdu_pool_size;
	g_iscsi.immediate_data_pool_size = opts->immediate_data_pool_size;
	g_iscsi.data_out_pool_size = opts->data_out_pool_size;
	/* [한국어] 스칼라 일괄 복사. */

	iscsi_log_globals();
	/* [한국어] 트레이스. */

	return 0;
}

/*
 * [한국어]
 * iscsi_set_discovery_auth - Discovery 세션의 CHAP 정책 변경 (RPC 노출).
 *
 * @return: 0 OK, -EINVAL (조합 모순).
 *
 * 4-튜플(disable, require, mutual, group)을 mutex 보호하에 갱신.
 */
int
iscsi_set_discovery_auth(bool disable_chap, bool require_chap, bool mutual_chap,
			 int32_t chap_group)
{
	if (!iscsi_check_chap_params(disable_chap, require_chap, mutual_chap,
				     chap_group)) {
		/* [한국어] 모순된 조합 거부. */
		SPDK_ERRLOG("CHAP params are illegal combination\n");
		return -EINVAL;
	}

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] 전역 락. */
	g_iscsi.disable_chap = disable_chap;
	g_iscsi.require_chap = require_chap;
	g_iscsi.mutual_chap = mutual_chap;
	g_iscsi.chap_group = chap_group;
	/* [한국어] 일괄 갱신. */
	pthread_mutex_unlock(&g_iscsi.mutex);

	return 0;
}

/*
 * [한국어]
 * iscsi_auth_group_add_secret - 인증 그룹에 (user, secret, muser, msecret) 시크릿 추가.
 *
 * @group: 대상 그룹.
 * @user/secret: CHAP user/secret (필수).
 * @muser/msecret: Mutual CHAP user/secret (옵션, 짝).
 * @return: 0 OK, -EINVAL/-EEXIST/-ENOMEM.
 *
 * 길이 한도 검증, user 중복 거부, calloc된 노드를 group->secret_head TAILQ에 추가.
 */
int
iscsi_auth_group_add_secret(struct spdk_iscsi_auth_group *group,
			    const char *user, const char *secret,
			    const char *muser, const char *msecret)
{
	struct spdk_iscsi_auth_secret *_secret;
	/* [한국어] 새 시크릿 노드. */
	size_t len;
	/* [한국어] 길이 검증용. */

	if (user == NULL || secret == NULL) {
		/* [한국어] CHAP 기본 한쪽이라도 누락은 EINVAL. */
		SPDK_ERRLOG("user and secret must be specified\n");
		return -EINVAL;
	}

	if (muser != NULL && msecret == NULL) {
		/* [한국어] muser는 msecret과 짝이어야 함. */
		SPDK_ERRLOG("msecret must be specified with muser\n");
		return -EINVAL;
	}

	TAILQ_FOREACH(_secret, &group->secret_head, tailq) {
		/* [한국어] 동일 그룹 내 user 중복 검사. */
		if (strcmp(_secret->user, user) == 0) {
			SPDK_ERRLOG("user for secret is duplicated\n");
			return -EEXIST;
		}
	}

	_secret = calloc(1, sizeof(*_secret));
	/* [한국어] 새 노드. */
	if (_secret == NULL) {
		SPDK_ERRLOG("calloc() failed for CHAP secret\n");
		return -ENOMEM;
	}

	len = strnlen(user, sizeof(_secret->user));
	/* [한국어] user 길이 측정 (배열 한도 초과 여부 검사용). */
	if (len > sizeof(_secret->user) - 1) {
		SPDK_ERRLOG("CHAP user longer than %zu characters: %s\n",
			    sizeof(_secret->user) - 1, user);
		free(_secret);
		return -EINVAL;
	}
	memcpy(_secret->user, user, len);
	/* [한국어] NUL은 calloc으로 이미 0. */

	len = strnlen(secret, sizeof(_secret->secret));
	if (len > sizeof(_secret->secret) - 1) {
		SPDK_ERRLOG("CHAP secret longer than %zu characters: %s\n",
			    sizeof(_secret->secret) - 1, secret);
		free(_secret);
		return -EINVAL;
	}
	memcpy(_secret->secret, secret, len);

	if (muser != NULL) {
		/* [한국어] Mutual CHAP 옵션. */
		len = strnlen(muser, sizeof(_secret->muser));
		if (len > sizeof(_secret->muser) - 1) {
			SPDK_ERRLOG("Mutual CHAP user longer than %zu characters: %s\n",
				    sizeof(_secret->muser) - 1, muser);
			free(_secret);
			return -EINVAL;
		}
		memcpy(_secret->muser, muser, len);

		len = strnlen(msecret, sizeof(_secret->msecret));
		if (len > sizeof(_secret->msecret) - 1) {
			SPDK_ERRLOG("Mutual CHAP secret longer than %zu characters: %s\n",
				    sizeof(_secret->msecret) - 1, msecret);
			free(_secret);
			return -EINVAL;
		}
		memcpy(_secret->msecret, msecret, len);
	}

	TAILQ_INSERT_TAIL(&group->secret_head, _secret, tailq);
	/* [한국어] 그룹의 secret 리스트에 추가. */
	return 0;
}

/*
 * [한국어]
 * iscsi_auth_group_delete_secret - 그룹에서 user 일치 시크릿 1개 제거.
 *
 * @return: 0 OK, -EINVAL(user NULL)/-ENODEV(미발견).
 */
int
iscsi_auth_group_delete_secret(struct spdk_iscsi_auth_group *group,
			       const char *user)
{
	struct spdk_iscsi_auth_secret *_secret;
	/* [한국어] 검색/제거 대상. */

	if (user == NULL) {
		SPDK_ERRLOG("user must be specified\n");
		return -EINVAL;
	}

	TAILQ_FOREACH(_secret, &group->secret_head, tailq) {
		/* [한국어] user 검색. */
		if (strcmp(_secret->user, user) == 0) {
			break;
		}
	}

	if (_secret == NULL) {
		SPDK_ERRLOG("secret is not found\n");
		return -ENODEV;
	}

	TAILQ_REMOVE(&group->secret_head, _secret, tailq);
	/* [한국어] 분리 후 free. */
	free(_secret);

	return 0;
}

/*
 * [한국어]
 * iscsi_add_auth_group - 새 인증 그룹 생성 후 g_iscsi.auth_group_head에 등록.
 *
 * @tag: 그룹 식별자 (>0, 0은 예약).
 * @_group: 출력 — 생성된 그룹 포인터.
 * @return: 0 OK, -EEXIST/-ENOMEM.
 */
int
iscsi_add_auth_group(int32_t tag, struct spdk_iscsi_auth_group **_group)
{
	struct spdk_iscsi_auth_group *group;
	/* [한국어] 새 그룹. */

	TAILQ_FOREACH(group, &g_iscsi.auth_group_head, tailq) {
		/* [한국어] tag 중복 검사. */
		if (group->tag == tag) {
			SPDK_ERRLOG("Auth group (%d) already exists\n", tag);
			return -EEXIST;
		}
	}

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		SPDK_ERRLOG("calloc() failed for auth group\n");
		return -ENOMEM;
	}

	TAILQ_INIT(&group->secret_head);
	/* [한국어] secret 리스트 초기화. */
	group->tag = tag;

	TAILQ_INSERT_TAIL(&g_iscsi.auth_group_head, group, tailq);
	/* [한국어] 전역 등록. */

	*_group = group;
	/* [한국어] 출력 인자에 새 그룹 반환. */
	return 0;
}

/*
 * [한국어]
 * iscsi_delete_auth_group - 그룹과 그 모든 secret 노드 free.
 *
 * 그룹은 g_iscsi.auth_group_head에서 분리되고, secret 리스트는 FOREACH_SAFE로 안전 순회 free.
 */
void
iscsi_delete_auth_group(struct spdk_iscsi_auth_group *group)
{
	struct spdk_iscsi_auth_secret *_secret, *tmp;
	/* [한국어] FOREACH_SAFE용. */

	TAILQ_REMOVE(&g_iscsi.auth_group_head, group, tailq);
	/* [한국어] 전역에서 분리. */

	TAILQ_FOREACH_SAFE(_secret, &group->secret_head, tailq, tmp) {
		/* [한국어] secret 모두 free. */
		TAILQ_REMOVE(&group->secret_head, _secret, tailq);
		free(_secret);
	}
	free(group);
	/* [한국어] 그룹 자체 free. */
}

/*
 * [한국어]
 * iscsi_find_auth_group_by_tag - tag로 그룹 검색.
 */
struct spdk_iscsi_auth_group *
iscsi_find_auth_group_by_tag(int32_t tag)
{
	struct spdk_iscsi_auth_group *group;
	/* [한국어] 순회용. */

	TAILQ_FOREACH(group, &g_iscsi.auth_group_head, tailq) {
		if (group->tag == tag) {
			return group;
		}
	}

	return NULL;
}

/*
 * [한국어]
 * iscsi_auth_groups_destroy - 모든 인증 그룹 free (셧다운 경로).
 */
static void
iscsi_auth_groups_destroy(void)
{
	struct spdk_iscsi_auth_group *group, *tmp;
	/* [한국어] FOREACH_SAFE용. */

	TAILQ_FOREACH_SAFE(group, &g_iscsi.auth_group_head, tailq, tmp) {
		iscsi_delete_auth_group(group);
	}
}

/*
 * [한국어]
 * iscsi_parse_auth_group - SPDK conf 섹션 1개를 파싱하여 인증 그룹 1개 생성.
 *
 * @sp: [AuthGroupN] 섹션.
 * @return: 0 OK, 음수 errno.
 *
 * "Auth user secret [muser msecret]" 라인 다수를 순회하며 시크릿 추가. 어느 한 라인이라도
 * 실패하면 그룹 자체를 delete (atomicity).
 */
static int
iscsi_parse_auth_group(struct spdk_conf_section *sp)
{
	int rc;
	int i;
	int tag;
	const char *val, *user, *secret, *muser, *msecret;
	struct spdk_iscsi_auth_group *group = NULL;
	/* [한국어] 새 그룹. */

	val = spdk_conf_section_get_val(sp, "Comment");
	if (val != NULL) {
		/* [한국어] Comment 라인은 진단 출력만. */
		SPDK_DEBUGLOG(iscsi, "Comment %s\n", val);
	}

	tag = spdk_conf_section_get_num(sp);
	/* [한국어] 섹션 이름 끝의 숫자(예: AuthGroup1 → 1). */

	rc = iscsi_add_auth_group(tag, &group);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to add auth group\n");
		return rc;
	}

	for (i = 0; ; i++) {
		/* [한국어] "Auth ..." 라인을 i=0,1,...로 순회. */
		val = spdk_conf_section_get_nval(sp, "Auth", i);
		if (val == NULL) {
			/* [한국어] 더 이상 라인 없음. */
			break;
		}

		user = spdk_conf_section_get_nmval(sp, "Auth", i, 0);
		secret = spdk_conf_section_get_nmval(sp, "Auth", i, 1);
		muser = spdk_conf_section_get_nmval(sp, "Auth", i, 2);
		msecret = spdk_conf_section_get_nmval(sp, "Auth", i, 3);
		/* [한국어] 라인 i의 토큰 0~3을 추출. */

		rc = iscsi_auth_group_add_secret(group, user, secret, muser, msecret);
		if (rc != 0) {
			/* [한국어] 어느 라인 실패 시 그룹 통째로 폐기. */
			SPDK_ERRLOG("Failed to add secret to auth group\n");
			iscsi_delete_auth_group(group);
			return rc;
		}
	}

	return 0;
}

/*
 * [한국어]
 * iscsi_parse_auth_info - g_iscsi.authfile에서 모든 [AuthGroupN] 섹션 파싱.
 *
 * @return: 0 OK, 음수 errno (파일 열기 또는 파싱 실패).
 *
 * 어느 그룹이라도 실패하면 이미 만든 모든 그룹을 destroy (atomicity).
 */
static int
iscsi_parse_auth_info(void)
{
	struct spdk_conf *config;
	struct spdk_conf_section *sp;
	int rc;

	config = spdk_conf_allocate();
	/* [한국어] SPDK conf 파서 객체 할당. */
	if (!config) {
		SPDK_ERRLOG("Failed to allocate config file\n");
		return -ENOMEM;
	}

	rc = spdk_conf_read(config, g_iscsi.authfile);
	/* [한국어] 파일 읽기. */
	if (rc != 0) {
		SPDK_INFOLOG(iscsi, "Failed to load auth file\n");
		spdk_conf_free(config);
		return rc;
	}

	sp = spdk_conf_first_section(config);
	/* [한국어] 첫 섹션. */
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "AuthGroup")) {
			/* [한국어] [AuthGroupN] 형식 섹션만 처리. */
			if (spdk_conf_section_get_num(sp) == 0) {
				/* [한국어] tag=0은 예약 — 거부. */
				SPDK_ERRLOG("Group 0 is invalid\n");
				iscsi_auth_groups_destroy();
				spdk_conf_free(config);
				return -EINVAL;
			}

			rc = iscsi_parse_auth_group(sp);
			if (rc != 0) {
				SPDK_ERRLOG("parse_auth_group() failed\n");
				iscsi_auth_groups_destroy();
				spdk_conf_free(config);
				return rc;
			}
		}
		sp = spdk_conf_next_section(sp);
		/* [한국어] 다음 섹션. */
	}

	spdk_conf_free(config);
	/* [한국어] conf 객체 free. */
	return 0;
}

/*
 * [한국어]
 * iscsi_find_auth_secret - (user, ag_tag)로 시크릿 검색.
 *
 * 그룹별 시크릿 리스트가 짧다고 가정한 O(N*M) 선형 탐색. 호출자 락 보유 가정.
 */
static struct spdk_iscsi_auth_secret *
iscsi_find_auth_secret(const char *authuser, int ag_tag)
{
	struct spdk_iscsi_auth_group *group;
	struct spdk_iscsi_auth_secret *_secret;

	TAILQ_FOREACH(group, &g_iscsi.auth_group_head, tailq) {
		/* [한국어] 모든 그룹 순회. */
		if (group->tag == ag_tag) {
			TAILQ_FOREACH(_secret, &group->secret_head, tailq) {
				/* [한국어] 그룹 내 시크릿 순회. */
				if (strcmp(_secret->user, authuser) == 0) {
					return _secret;
				}
			}
		}
	}

	return NULL;
}

/*
 * [한국어]
 * iscsi_chap_get_authinfo - 로그인 phase 인증 단계에서 (authuser, ag_tag)로 시크릿 복사.
 *
 * @auth: 출력 — iscsi_chap_auth 구조체.
 * @authuser: 인증 요청한 user.
 * @ag_tag: 사용할 인증 그룹 tag.
 * @return: 0 OK, -EINVAL(user NULL), -ENOENT(미발견).
 *
 * iSCSI 로그인 보안 협상에서 호출 — 시크릿 비교를 위해 정답을 가져온다.
 * mutex 보호하에 시크릿 복사 (그룹 변경과의 race 방지).
 */
int
iscsi_chap_get_authinfo(struct iscsi_chap_auth *auth, const char *authuser,
			int ag_tag)
{
	struct spdk_iscsi_auth_secret *_secret;
	/* [한국어] find 결과. */

	if (authuser == NULL) {
		return -EINVAL;
	}

	if (auth->user[0] != '\0') {
		/* [한국어] 이전 시도가 남긴 데이터 클리어 — 새 시도. */
		memset(auth->user, 0, sizeof(auth->user));
		memset(auth->secret, 0, sizeof(auth->secret));
		memset(auth->muser, 0, sizeof(auth->muser));
		memset(auth->msecret, 0, sizeof(auth->msecret));
	}

	pthread_mutex_lock(&g_iscsi.mutex);
	/* [한국어] auth_group_head 보호. */

	_secret = iscsi_find_auth_secret(authuser, ag_tag);
	if (_secret == NULL) {
		pthread_mutex_unlock(&g_iscsi.mutex);

		SPDK_ERRLOG("CHAP secret is not found: user:%s, tag:%d\n",
			    authuser, ag_tag);
		return -ENOENT;
	}

	memcpy(auth->user, _secret->user, sizeof(auth->user));
	memcpy(auth->secret, _secret->secret, sizeof(auth->secret));
	/* [한국어] CHAP 기본 user/secret 복사. */

	if (_secret->muser[0] != '\0') {
		/* [한국어] Mutual CHAP 설정된 경우만 추가 복사. */
		memcpy(auth->muser, _secret->muser, sizeof(auth->muser));
		memcpy(auth->msecret, _secret->msecret, sizeof(auth->msecret));
	}

	pthread_mutex_unlock(&g_iscsi.mutex);
	return 0;
}

/*
 * [한국어]
 * iscsi_initialize_global_params - g_spdk_iscsi_opts → g_iscsi 적용 후 opts free.
 *
 * 호출 후 g_spdk_iscsi_opts는 NULL이 되어 RPC iscsi_set_options를 다시 받지 않는다 (post-init).
 */
static int
iscsi_initialize_global_params(void)
{
	int rc;

	if (!g_spdk_iscsi_opts) {
		/* [한국어] RPC로 미리 설정되지 않았으면 기본값으로 alloc. */
		g_spdk_iscsi_opts = iscsi_opts_alloc();
		if (!g_spdk_iscsi_opts) {
			SPDK_ERRLOG("iscsi_opts_alloc_failed() failed\n");
			return -ENOMEM;
		}
	}

	rc = iscsi_set_global_params(g_spdk_iscsi_opts);
	if (rc != 0) {
		SPDK_ERRLOG("iscsi_set_global_params() failed\n");
	}

	iscsi_opts_free(g_spdk_iscsi_opts);
	/* [한국어] 적용 완료 — opts 객체 free. */
	g_spdk_iscsi_opts = NULL;
	/* [한국어] 후속 set_options 차단. */

	return rc;
}

/*
 * [한국어]
 * iscsi_init_complete - 초기화 콜백 발사 (1회).
 *
 * cb_fn/cb_arg를 로컬로 옮기고 전역을 NULL로 리셋한 뒤 호출 — 콜백이 다시 spdk_iscsi_init를
 * 호출해도 안전.
 */
static void
iscsi_init_complete(int rc)
{
	spdk_iscsi_init_cb cb_fn = g_init_cb_fn;
	void *cb_arg = g_init_cb_arg;

	g_init_cb_fn = NULL;
	g_init_cb_arg = NULL;

	cb_fn(cb_arg, rc);
}

/*
 * [한국어]
 * iscsi_parse_configuration - authfile이 있으면 파싱, 그 후 init_complete.
 *
 * 모든 poll_group 생성이 완료된 후 호출되는 마지막 단계.
 */
static void
iscsi_parse_configuration(void)
{
	int rc = 0;

	if (g_iscsi.authfile != NULL) {
		if (access(g_iscsi.authfile, R_OK) == 0) {
			/* [한국어] 파일 존재 + 읽기 권한 — 파싱. */
			rc = iscsi_parse_auth_info();
			if (rc < 0) {
				SPDK_ERRLOG("iscsi_parse_auth_info() failed\n");
			}
		} else {
			/* [한국어] 권한/존재 없음 — info 로그 후 정상 진행. */
			SPDK_INFOLOG(iscsi, "CHAP secret file is not found in the path %s\n",
				     g_iscsi.authfile);
		}
	}

	iscsi_init_complete(rc);
	/* [한국어] 콜백 호출 — SPDK app가 다음 subsystem으로 진행. */
}

/*
 * [한국어]
 * iscsi_poll_group_poll - poll_group의 핵심 polling 함수 (SPDK_POLLER_REGISTER 콜백).
 *
 * @ctx: spdk_iscsi_poll_group*.
 * @return: SPDK_POLLER_BUSY/IDLE.
 *
 * 1) connections STAILQ가 비었으면 IDLE.
 * 2) sock_group_poll로 모든 conn의 readable/writable 이벤트 처리.
 * 3) EXITING 상태인 conn을 destruct.
 * 실행 컨텍스트: 각 코어의 SPDK thread polling 루프.
 */
static int
iscsi_poll_group_poll(void *ctx)
{
	struct spdk_iscsi_poll_group *group = ctx;
	struct spdk_iscsi_conn *conn, *tmp;
	int rc;

	if (spdk_unlikely(STAILQ_EMPTY(&group->connections))) {
		/* [한국어] 빈 그룹은 IDLE — reactor가 다른 work에 양보. */
		return SPDK_POLLER_IDLE;
	}

	rc = spdk_sock_group_poll(group->sock_group);
	/* [한국어] 등록된 모든 conn 소켓을 한 번 polling — readable conn은 콜백 트리거. */
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_poll() failed, sock_group=%p, rc %d: %s\n", group->sock_group, rc,
			    spdk_strerror(-rc));
	}

	STAILQ_FOREACH_SAFE(conn, &group->connections, pg_link, tmp) {
		/* [한국어] EXITING 상태의 conn 정리. */
		if (conn->state == ISCSI_CONN_STATE_EXITING) {
			iscsi_conn_destruct(conn);
		}
	}

	return rc != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
	/* [한국어] 작업 수행 시 BUSY 반환 (reactor 통계용). */
}

/*
 * [한국어]
 * iscsi_poll_group_handle_nop - NopIn poller (1초 주기).
 *
 * 모든 conn에 대해 nop 핸들링 (timeout 검사, NopIn 송신 등). conn.c에 위임.
 */
static int
iscsi_poll_group_handle_nop(void *ctx)
{
	struct spdk_iscsi_poll_group *group = ctx;
	struct spdk_iscsi_conn *conn, *tmp;

	STAILQ_FOREACH_SAFE(conn, &group->connections, pg_link, tmp) {
		iscsi_conn_handle_nop(conn);
		/* [한국어] conn별 nop 상태 머신 진행. */
	}

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * iscsi_poll_group_create - SPDK io_device의 채널 생성 콜백.
 *
 * @io_device: &g_iscsi.
 * @ctx_buf: 채널 컨텍스트 buffer (spdk_iscsi_poll_group 크기로 register됨).
 *
 * 각 SPDK thread에서 spdk_get_io_channel(&g_iscsi)을 처음 호출할 때 트리거.
 * sock_group, polling poller(0us=매 tick), nop poller(1s) 등록.
 */
static int
iscsi_poll_group_create(void *io_device, void *ctx_buf)
{
	struct spdk_iscsi_poll_group *pg = ctx_buf;
	/* [한국어] ctx_buf를 poll_group으로 캐스팅. */

	STAILQ_INIT(&pg->connections);
	/* [한국어] connections 리스트 초기화. */
	pg->sock_group = spdk_sock_group_create(NULL);
	/* [한국어] sock_group 생성. ctx=NULL — 그룹 콜백에서 별도 ctx 불필요. */
	assert(pg->sock_group != NULL);

	pg->poller = SPDK_POLLER_REGISTER(iscsi_poll_group_poll, pg, 0);
	/* [한국어] period=0 → 매 reactor tick마다 호출 (busy poll). */
	/* set the period to 1 sec */
	pg->nop_poller = SPDK_POLLER_REGISTER(iscsi_poll_group_handle_nop, pg, 1000000);
	/* [한국어] 1초 (1,000,000us) 주기. NopIn 검사. */

	return 0;
}

/*
 * [한국어]
 * iscsi_poll_group_destroy - SPDK io_device 채널 소멸 콜백.
 *
 * 모든 conn이 분리된 후 호출. sock_group close, poller 해제 후 spdk_thread_exit로
 * 이 채널이 속한 SPDK thread도 종료시킨다.
 */
static void
iscsi_poll_group_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_iscsi_poll_group *pg = ctx_buf;
	struct spdk_io_channel *ch;
	struct spdk_thread *thread;

	assert(pg->poller != NULL);
	assert(pg->sock_group != NULL);

	spdk_sock_group_close(&pg->sock_group);
	/* [한국어] 소켓 그룹 close 및 NULL 포인터 안전. */
	spdk_poller_unregister(&pg->poller);
	spdk_poller_unregister(&pg->nop_poller);

	ch = spdk_io_channel_from_ctx(pg);
	/* [한국어] ctx_buf로부터 io_channel 복원. */
	thread = spdk_io_channel_get_thread(ch);
	/* [한국어] 이 채널이 속한 SPDK thread. */

	assert(thread == spdk_get_thread());
	/* [한국어] destroy는 해당 thread 자체에서 호출되어야 함 (lockless invariant). */

	spdk_thread_exit(thread);
	/* [한국어] thread 종료 요청 — 이후 polling 루프가 자연스럽게 종료. */
}

/*
 * [한국어]
 * _iscsi_init_thread_done - poll_group 생성이 끝나면 init_thread로 전송되는 메시지 핸들러.
 *
 * @ctx: 생성된 poll_group.
 *
 * 모든 코어의 poll_group이 완성될 때까지 (refcnt 카운트다운) 대기 후 마지막에
 * iscsi_parse_configuration → init_complete.
 */
static void
_iscsi_init_thread_done(void *ctx)
{
	struct spdk_iscsi_poll_group *pg = ctx;

	TAILQ_INSERT_TAIL(&g_iscsi.poll_group_head, pg, link);
	/* [한국어] 전역 poll_group 리스트에 등록. */
	if (--g_iscsi.refcnt == 0) {
		/* [한국어] 마지막 poll_group까지 완료 — 다음 단계. */
		iscsi_parse_configuration();
	}
}

/*
 * [한국어]
 * _iscsi_init_thread - 각 코어 SPDK thread에서 처음 실행되는 메시지 핸들러.
 *
 * io_channel을 획득하여 poll_group 인스턴스를 받고, 결과를 init_thread로 전송.
 */
static void
_iscsi_init_thread(void *ctx)
{
	struct spdk_io_channel *ch;
	struct spdk_iscsi_poll_group *pg;

	ch = spdk_get_io_channel(&g_iscsi);
	/* [한국어] 채널 획득 — 첫 호출이면 iscsi_poll_group_create 트리거. */
	pg = spdk_io_channel_get_ctx(ch);
	/* [한국어] 채널의 ctx_buf = poll_group. */

	spdk_thread_send_msg(g_init_thread, _iscsi_init_thread_done, pg);
	/* [한국어] init_thread로 결과 전송 — 직렬화. */
}

/*
 * [한국어]
 * initialize_iscsi_poll_group - SPDK io_device 등록 + 코어별 thread 생성.
 *
 * 1) spdk_io_device_register: poll_group을 SPDK io_device로 노출 (per-thread channel).
 * 2) 활성 코어 각각에 새 SPDK thread 생성 후 _iscsi_init_thread 메시지 송신.
 * 3) refcnt를 코어 수만큼 누적하여 모든 thread 완료를 대기.
 *
 * 호출 체인:
 *   iscsi_parse_globals → [initialize_iscsi_poll_group]
 */
static void
initialize_iscsi_poll_group(void)
{
	struct spdk_cpuset tmp_cpumask = {};
	uint32_t i;
	char thread_name[32];
	struct spdk_thread *thread;

	spdk_io_device_register(&g_iscsi, iscsi_poll_group_create, iscsi_poll_group_destroy,
				sizeof(struct spdk_iscsi_poll_group), "iscsi_tgt");
	/* [한국어] &g_iscsi를 io_device로 등록 — 각 thread에서 채널 획득 시 ctor/dtor 호출. */

	/* Create threads for CPU cores active for this application, and send a
	 * message to each thread to create a poll group on it.
	 */
	g_init_thread = spdk_get_thread();
	/* [한국어] 현재(=초기화) thread를 g_init_thread로 보관. */
	assert(g_init_thread != NULL);
	assert(g_iscsi.refcnt == 0);

	SPDK_ENV_FOREACH_CORE(i) {
		/* [한국어] 활성 CPU 코어 i 각각에 대해. */
		spdk_cpuset_zero(&tmp_cpumask);
		spdk_cpuset_set_cpu(&tmp_cpumask, i, true);
		/* [한국어] 단일 코어 mask 준비 — 새 thread를 그 코어에 affinity. */
		snprintf(thread_name, sizeof(thread_name), "iscsi_poll_group_%u", i);

		thread = spdk_thread_create(thread_name, &tmp_cpumask);
		/* [한국어] 새 SPDK thread 생성 — 코어 i에서만 실행. */
		assert(thread != NULL);

		g_iscsi.refcnt++;
		/* [한국어] 완료 대기 카운터. */
		spdk_thread_send_msg(thread, _iscsi_init_thread, NULL);
		/* [한국어] 새 thread에 init 메시지 송신 — lockless 메시지 큐. */
	}
}

/*
 * [한국어]
 * iscsi_parse_globals - 글로벌 옵션 + 풀 + conn + poll_group 일괄 초기화.
 *
 * 단계: opts 적용 → session 배열 calloc → MaxConnections 설정 → 풀 생성 → conn 모듈
 * 초기화 → poll_group 초기화 (마지막 단계는 비동기, parse_configuration이 마지막에 호출됨).
 */
static int
iscsi_parse_globals(void)
{
	int rc;

	rc = iscsi_initialize_global_params();
	if (rc != 0) {
		SPDK_ERRLOG("iscsi_initialize_iscsi_global_params() failed\n");
		return rc;
	}

	g_iscsi.session = calloc(1, sizeof(struct spdk_iscsi_sess *) * g_iscsi.MaxSessions);
	/* [한국어] tsih→sess 매핑 배열. session_pool ctor에서 채워진다. */
	if (!g_iscsi.session) {
		SPDK_ERRLOG("calloc() failed for session array\n");
		return -1;
	}

	/*
	 * For now, just support same number of total connections, rather
	 *  than MaxSessions * MaxConnectionsPerSession.  After we add better
	 *  handling for low resource conditions from our various buffer
	 *  pools, we can bump this up to support more connections.
	 */
	g_iscsi.MaxConnections = g_iscsi.MaxSessions;
	/* [한국어] 보수적: 세션 수 = 연결 수. 풀 자원 부족 처리 개선 후 상향 가능. */

	rc = iscsi_initialize_all_pools();
	if (rc != 0) {
		SPDK_ERRLOG("initialize_all_pools() failed\n");
		free(g_iscsi.session);
		g_iscsi.session = NULL;
		return -1;
	}

	rc = initialize_iscsi_conns();
	/* [한국어] conn 모듈 초기화 (conn_pool 등). */
	if (rc < 0) {
		SPDK_ERRLOG("initialize_iscsi_conns() failed\n");
		free(g_iscsi.session);
		g_iscsi.session = NULL;
		return rc;
	}

	initialize_iscsi_poll_group();
	/* [한국어] 비동기 — 완료는 _iscsi_init_thread_done에서. */
	return 0;
}

/*
 * [한국어]
 * spdk_iscsi_init - iSCSI 라이브러리 초기화 진입점 (외부 API).
 *
 * @cb_fn: 완료 콜백 (필수).
 * @cb_arg: 콜백 인자.
 *
 * 비동기: 호출 즉시 반환. 모든 단계가 완료되면 cb_fn(cb_arg, rc)가 g_init_thread에서 호출.
 */
void
spdk_iscsi_init(spdk_iscsi_init_cb cb_fn, void *cb_arg)
{
	int rc;

	assert(cb_fn != NULL);
	g_init_cb_fn = cb_fn;
	g_init_cb_arg = cb_arg;
	/* [한국어] 콜백 보관 — 비동기 흐름의 마지막에 호출. */

	rc = iscsi_parse_globals();
	if (rc < 0) {
		/* [한국어] 동기 단계 실패 — 즉시 콜백으로 실패 통지. */
		SPDK_ERRLOG("iscsi_parse_globals() failed\n");
		iscsi_init_complete(-1);
	}

	/*
	 * iscsi_parse_configuration() will be called as the callback to
	 * spdk_initialize_iscsi_poll_group() and will complete iSCSI
	 * subsystem initialization.
	 */
}

/*
 * [한국어]
 * spdk_iscsi_fini - iSCSI 라이브러리 종료 진입점 (외부 API).
 *
 * 비동기: portal listen 중단 → conn 셧다운 시퀀스 시작. shutdown_iscsi_conns_done이
 * conn 모두 정리되면 호출되어 fini_dev_unreg → io_device unregister → cb_fn.
 */
void
spdk_iscsi_fini(spdk_iscsi_fini_cb cb_fn, void *cb_arg)
{
	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	iscsi_portal_grp_close_all();
	/* [한국어] 신규 accept 차단. */
	shutdown_iscsi_conns();
	/* [한국어] 기존 conn 모두 EXITING 상태로 전환 (poll_group_poll에서 destruct). */
}

/*
 * [한국어]
 * iscsi_fini_done - 마지막 fini 콜백 발사.
 */
static void
iscsi_fini_done(void *io_device)
{
	g_fini_cb_fn(g_fini_cb_arg);
}

/*
 * [한국어]
 * _iscsi_fini_dev_unreg - 모든 채널 분리 후 호출되는 spdk_for_each_channel 완료 콜백.
 *
 * 풀 leak 검사 → 풀 free → tgt_node/IG/PG/auth_group 일괄 destroy → mutex destroy →
 * io_device unregister → cb_fn.
 */
static void
_iscsi_fini_dev_unreg(struct spdk_io_channel_iter *i, int status)
{
	iscsi_check_pools();
	/* [한국어] leak 진단. */
	iscsi_free_pools();
	free(g_iscsi.session);

	assert(TAILQ_EMPTY(&g_iscsi.poll_group_head));
	/* [한국어] 모든 poll_group이 분리되어 있어야 함. */

	iscsi_shutdown_tgt_nodes();
	iscsi_init_grps_destroy();
	iscsi_portal_grps_destroy();
	iscsi_auth_groups_destroy();
	/* [한국어] 메타데이터 일괄 해제. */

	free(g_iscsi.authfile);
	free(g_iscsi.nodebase);

	pthread_mutex_destroy(&g_iscsi.mutex);
	if (g_init_thread != NULL) {
		/* g_init_thread is set just after the io_device is
		 * registered, so we can use it to determine if it
		 * needs to be unregistered (in cases where iscsi init
		 * fails).
		 */
		/* [한국어] init이 io_device까지 완료된 경우만 unregister. */
		spdk_io_device_unregister(&g_iscsi, iscsi_fini_done);
	} else {
		/* [한국어] init 초반 실패 — 직접 cb_fn 호출. */
		iscsi_fini_done(NULL);
	}
}

/*
 * [한국어]
 * _iscsi_fini_thread - spdk_for_each_channel이 각 채널에서 호출하는 핸들러.
 *
 * 자기 채널의 poll_group을 g_iscsi.poll_group_head에서 분리하고 채널 ref drop →
 * poll_group_destroy 트리거 (refcnt 0 도달 시).
 */
static void
_iscsi_fini_thread(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch;
	struct spdk_iscsi_poll_group *pg;

	ch = spdk_io_channel_iter_get_channel(i);
	pg = spdk_io_channel_get_ctx(ch);

	pthread_mutex_lock(&g_iscsi.mutex);
	TAILQ_REMOVE(&g_iscsi.poll_group_head, pg, link);
	/* [한국어] 전역에서 분리. */
	pthread_mutex_unlock(&g_iscsi.mutex);

	spdk_put_io_channel(ch);
	/* [한국어] ref drop → 마지막이면 poll_group_destroy 트리거. */

	spdk_for_each_channel_continue(i, 0);
	/* [한국어] 다음 채널로 진행. */
}

/*
 * [한국어]
 * shutdown_iscsi_conns_done - conn.c가 모든 conn 정리 완료 후 호출하는 hook.
 *
 * 각 채널을 순회하며 poll_group을 정리한다.
 */
void
shutdown_iscsi_conns_done(void)
{
	spdk_for_each_channel(&g_iscsi, _iscsi_fini_thread, NULL, _iscsi_fini_dev_unreg);
	/* [한국어] 모든 채널 순회 후 _iscsi_fini_dev_unreg 호출. */
}

/*
 * [한국어]
 * iscsi_opts_info_json - 전역 iSCSI 옵션을 JSON 객체로 직렬화.
 *
 * RPC iscsi_get_options 응답이나 save_config의 params 부분에 사용.
 */
void
iscsi_opts_info_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	if (g_iscsi.authfile != NULL) {
		spdk_json_write_named_string(w, "auth_file", g_iscsi.authfile);
	}
	spdk_json_write_named_string(w, "node_base", g_iscsi.nodebase);

	spdk_json_write_named_uint32(w, "max_sessions", g_iscsi.MaxSessions);
	spdk_json_write_named_uint32(w, "max_connections_per_session",
				     g_iscsi.MaxConnectionsPerSession);

	spdk_json_write_named_uint32(w, "max_queue_depth", g_iscsi.MaxQueueDepth);

	spdk_json_write_named_uint32(w, "default_time2wait", g_iscsi.DefaultTime2Wait);
	spdk_json_write_named_uint32(w, "default_time2retain", g_iscsi.DefaultTime2Retain);

	spdk_json_write_named_uint32(w, "first_burst_length", g_iscsi.FirstBurstLength);

	spdk_json_write_named_bool(w, "immediate_data", g_iscsi.ImmediateData);

	spdk_json_write_named_bool(w, "allow_duplicated_isid", g_iscsi.AllowDuplicateIsid);

	spdk_json_write_named_uint32(w, "error_recovery_level", g_iscsi.ErrorRecoveryLevel);

	spdk_json_write_named_int32(w, "nop_timeout", g_iscsi.timeout);
	spdk_json_write_named_int32(w, "nop_in_interval", g_iscsi.nopininterval);

	spdk_json_write_named_bool(w, "disable_chap", g_iscsi.disable_chap);
	spdk_json_write_named_bool(w, "require_chap", g_iscsi.require_chap);
	spdk_json_write_named_bool(w, "mutual_chap", g_iscsi.mutual_chap);
	spdk_json_write_named_int32(w, "chap_group", g_iscsi.chap_group);

	spdk_json_write_named_uint32(w, "max_large_datain_per_connection",
				     g_iscsi.MaxLargeDataInPerConnection);
	spdk_json_write_named_uint32(w, "max_r2t_per_connection",
				     g_iscsi.MaxR2TPerConnection);

	spdk_json_write_named_uint32(w, "pdu_pool_size", g_iscsi.pdu_pool_size);
	spdk_json_write_named_uint32(w, "immediate_data_pool_size",
				     g_iscsi.immediate_data_pool_size);
	spdk_json_write_named_uint32(w, "data_out_pool_size", g_iscsi.data_out_pool_size);

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * iscsi_auth_group_info_json - 단일 인증 그룹의 상태 JSON 직렬화.
 *
 * 형식: { "tag": int, "secrets": [ {user, secret, [muser, msecret]} ... ] }.
 * Mutual CHAP은 muser가 비어있지 않을 때만 출력.
 */
static void
iscsi_auth_group_info_json(struct spdk_iscsi_auth_group *group,
			   struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_auth_secret *_secret;
	/* [한국어] 시크릿 순회용. */

	spdk_json_write_object_begin(w);

	spdk_json_write_named_int32(w, "tag", group->tag);

	spdk_json_write_named_array_begin(w, "secrets");
	TAILQ_FOREACH(_secret, &group->secret_head, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "user", _secret->user);
		spdk_json_write_named_string(w, "secret", _secret->secret);

		if (_secret->muser[0] != '\0') {
			/* [한국어] Mutual 설정된 시크릿만. */
			spdk_json_write_named_string(w, "muser", _secret->muser);
			spdk_json_write_named_string(w, "msecret", _secret->msecret);
		}

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * iscsi_auth_group_config_json - 인증 그룹을 재생성 RPC method/params 형태로 직렬화.
 */
static void
iscsi_auth_group_config_json(struct spdk_iscsi_auth_group *group,
			     struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "iscsi_create_auth_group");
	/* [한국어] RPC 명. */

	spdk_json_write_name(w, "params");
	iscsi_auth_group_info_json(group, w);
	/* [한국어] info를 그대로 params로 사용 — replay 가능. */

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * iscsi_auth_groups_info_json - 모든 인증 그룹 정보 JSON 출력.
 */
void
iscsi_auth_groups_info_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_auth_group *group;

	TAILQ_FOREACH(group, &g_iscsi.auth_group_head, tailq) {
		iscsi_auth_group_info_json(group, w);
	}
}

/*
 * [한국어]
 * iscsi_auth_groups_config_json - 모든 인증 그룹 재생성 RPC 형태 출력.
 */
static void
iscsi_auth_groups_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_auth_group *group;

	TAILQ_FOREACH(group, &g_iscsi.auth_group_head, tailq) {
		iscsi_auth_group_config_json(group, w);
	}
}

/*
 * [한국어]
 * iscsi_opts_config_json - 전역 옵션을 iscsi_set_options method/params 형태로 직렬화.
 */
static void
iscsi_opts_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "iscsi_set_options");

	spdk_json_write_name(w, "params");
	iscsi_opts_info_json(w);

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * spdk_iscsi_config_json - SPDK save_config의 iSCSI subsystem 진입점.
 *
 * 출력: [ {set_options}, {portal_groups…}, {init_groups…}, {tgt_nodes…}, {auth_groups…} ]
 * 이 결과를 그대로 다시 RPC로 replay하면 동일 상태를 재구성 가능.
 */
void
spdk_iscsi_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_array_begin(w);
	iscsi_opts_config_json(w);
	iscsi_portal_grps_config_json(w);
	iscsi_init_grps_config_json(w);
	iscsi_tgt_nodes_config_json(w);
	iscsi_auth_groups_config_json(w);
	spdk_json_write_array_end(w);
}

SPDK_LOG_REGISTER_COMPONENT(iscsi)
/* [한국어] SPDK 로그 컴포넌트 등록 — SPDK_DEBUGLOG(iscsi, ...) 사용 가능. */
