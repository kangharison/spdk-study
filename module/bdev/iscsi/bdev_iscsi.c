/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] iSCSI initiator 기반 bdev 모듈 본체 (bdev_iscsi.c)
 *
 * === 파일의 역할 ===
 * 외부 iSCSI target (네트워크 너머의 LUN) 을 SPDK 의 가상 블록 디바이스(bdev) 로
 * 노출시키는 모듈이다. libiscsi 라는 외부 라이브러리를 사용해 TCP 위에서 iSCSI
 * 프로토콜(login phase → full-feature phase) 을 수행하고, 연결이 완료된 LUN 에
 * 대해 spdk_bdev I/O 요청을 SCSI CDB (READ16/WRITE16/SYNCHRONIZE_CACHE16/UNMAP) 로
 * 변환해 송신한다. 응답은 비동기 콜백 (bdev_iscsi_command_cb 등) 으로 받아
 * SCSI status 와 sense data 를 spdk_bdev_io_status / sense 로 매핑한다. 모듈
 * 라이프사이클(create/destroy), 채널 라이프사이클(get_io_channel/destroy_cb),
 * polling (libiscsi 의 fd 를 poll(2) 로 감시) 까지 모두 이 파일이 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK I/O 스택의 "bdev 백엔드 모듈" 계층이다. 호출 체인:
 *   상위 application → spdk_bdev_read/write (lib/bdev) →
 *   bdev_iscsi_submit_request (이 파일) → libiscsi (iscsi_read16_task 등) →
 *   TCP 소켓 → 원격 iSCSI target → 원격 SCSI 디바이스.
 * SPDK reactor 가 무한 polling 루프에서 bdev_iscsi_poll_lun() 을 호출하여
 * libiscsi 의 fd 를 poll() 하고, 응답이 도착하면 iscsi_service() 가 콜백을
 * 실행한다. 즉 polled-mode 이지만 실제 데이터 송수신은 커널 TCP 스택을 통하므로
 * NVMe 유저스페이스 드라이버처럼 완전한 kernel-bypass 는 아니다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/bdev: spdk_bdev_module 등록(SPDK_BDEV_MODULE_REGISTER), spdk_bdev_register,
 *   spdk_bdev_io_complete 등을 통해 bdev 코어에 연결.
 * - lib/thread: SPDK_POLLER_REGISTER, spdk_thread_send_msg, spdk_get_io_channel
 *   등으로 reactor/스레드 인프라 사용.
 * - libiscsi (외부): iscsi_create_context, iscsi_full_connect_async,
 *   iscsi_readNN_task / iscsi_writeNN_task / iscsi_unmap_task /
 *   iscsi_synchronizecache16_task / iscsi_task_mgmt_lun_reset_async 등 사용.
 * - bdev_iscsi.h: 공개 인터페이스(create_iscsi_disk / delete_iscsi_disk /
 *   bdev_iscsi_get_opts / bdev_iscsi_set_opts) 노출.
 * - bdev_iscsi_rpc.c: RPC 핸들러에서 본 파일의 create/delete API 를 호출.
 * 데이터 흐름: spdk_bdev_io (iovec, lba, num_blocks) → struct scsi_task (CDB) →
 * libiscsi 가 TCP socket 으로 송신 → target 응답 → bdev_iscsi_command_cb →
 * spdk_bdev_io_complete_scsi_status() 로 sense 데이터까지 상위에 전달.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct bdev_iscsi_lun: LUN 1개당 1개. iscsi_context, lun_id, main_td(이 LUN
 *   을 polling 하는 스레드), no_main_ch_poller(채널이 없을 때 background polling),
 *   unmap_supported 등을 보관.
 * - struct bdev_iscsi_io: bdev_io 의 driver_ctx 영역에 임베드되는 per-IO 컨텍스트.
 *   submit_td(요청을 보낸 스레드), scsi_status, sense key/asc/ascq.
 * - struct bdev_iscsi_conn_req: 비동기 연결 진행 상태(login phase → INQUIRY →
 *   READ_CAPACITY → create_iscsi_lun) 를 추적.
 * - create_iscsi_disk(): 외부에서 호출되는 진입점. 컨텍스트 만들고 비동기 연결
 *   시작. g_iscsi_conn_req 에 등록되어 g_conn_poller 가 진행을 폴링.
 * - bdev_iscsi_submit_request(): bdev 코어가 호출. iscsi_io->submit_td 저장 후
 *   main_td 로 전달(crossing thread 시 spdk_thread_send_msg 사용).
 * - bdev_iscsi_poll_lun(): main_td 의 poller. libiscsi fd 를 poll(2) → revents 가
 *   있으면 iscsi_service() 로 응답 처리.
 * - bdev_iscsi_command_cb(): READ/WRITE/FLUSH/UNMAP 공통 완료 콜백. sense key
 *   가 CAPACITY_DATA_HAS_CHANGED 면 READ_CAPACITY16 재발행 후 재시도.
 */

#include "spdk/stdinc.h"          /* [한국어] 표준 C 헤더 묶음 (size_t, NULL, calloc, free, errno 등). */

#include "spdk/bdev.h"            /* [한국어] spdk_bdev / spdk_bdev_io / spdk_bdev_register API. */
#include "spdk/env.h"             /* [한국어] DPDK 환경 추상화 — 직접 사용은 적지만 메모리 할당 API 노출. */
#include "spdk/fd.h"              /* [한국어] fd 유틸리티 — fd 기반 유틸 함수. */
#include "spdk/thread.h"          /* [한국어] spdk_thread, spdk_poller, spdk_thread_send_msg. */
#include "spdk/json.h"            /* [한국어] config_json 출력에 사용되는 JSON writer. */
#include "spdk/util.h"            /* [한국어] spdk_divide_round_up, spdk_min, SPDK_COUNTOF 등 헬퍼. */
#include "spdk/rpc.h"             /* [한국어] RPC 등록(SPDK_RPC_REGISTER). 이 파일은 직접 사용 안 하지만
                                   *         spdk_jsonrpc_request 등 포워드 선언이 필요할 수 있어 포함. */
#include "spdk/string.h"          /* [한국어] spdk_strerror, strdup 래퍼 등. */
#include "spdk/iscsi_spec.h"      /* [한국어] iSCSI 프로토콜 상수 — ISCSI_HEADER_DIGEST_NONE,
                                   *         ISCSI_IMMEDIATE_DATA_NO, ISCSI_TASK_FUNC_RESP_COMPLETE 등. */

#include "spdk/log.h"             /* [한국어] SPDK_ERRLOG / SPDK_DEBUGLOG / SPDK_LOG_REGISTER_COMPONENT. */
#include "spdk/bdev_module.h"     /* [한국어] bdev 모듈 개발자용 헤더 — spdk_bdev_module 구조체,
                                   *         spdk_bdev_io_complete_scsi_status, get_buf 콜백 등. */

#include "iscsi/iscsi.h"          /* [한국어] libiscsi 공개 API — iscsi_context, iscsi_create_context,
                                   *         iscsi_readNN_task, iscsi_get_fd, iscsi_service 등. */
#include "iscsi/scsi-lowlevel.h"  /* [한국어] libiscsi 의 SCSI 하위 인터페이스 — scsi_task, sense data,
                                   *         scsi_readcapacity16, scsi_inquiry_block_limits 등. */

#include "bdev_iscsi.h"           /* [한국어] 본 모듈의 공개 인터페이스 — create_iscsi_disk,
                                   *         delete_iscsi_disk, spdk_bdev_iscsi_opts 등 선언. */

struct bdev_iscsi_lun;        /* [한국어] forward declaration — readcapacity16/free_lun 등이 LUN 포인터를
                               *         사용하지만, 구조체 본체는 아래에 정의된다. */

#define BDEV_ISCSI_CONNECTION_POLL_US 500 /* 0.5 ms */
/* [한국어] connection 폴링 주기 (마이크로초). iSCSI login phase 가 진행 중일 때
 * g_conn_poller 가 0.5ms 마다 libiscsi fd 를 poll() 한다. login 은 빠르게 끝내야
 * 사용자 응답 지연이 적기 때문에 짧은 주기를 사용. */
#define BDEV_ISCSI_NO_MAIN_CH_POLL_US 10000 /* 10ms */
/* [한국어] LUN 에 I/O 채널이 없는 동안 background polling 주기. 이 때는 I/O 응답이
 * 거의 없으므로 CPU 절약을 위해 10ms 로 느슨하게 설정. unmap/resize 같은 비동기
 * 요청이 백그라운드에 남아 있어 완전 0 으로 둘 수는 없음. */

#define BDEV_ISCSI_TIMEOUT_POLL_PERIOD_DEFAULT	1000000ULL /* 1 s */
/* [한국어] libiscsi 의 internal timeout check 폴러 주기 기본값(us) — 1초. */
#define BDEV_ISCSI_TIMEOUT_DEFAULT 30 /* 30 s */
/* [한국어] 개별 SCSI 명령의 응답 대기 timeout 기본값(초) — 30초. */
#define BDEV_ISCSI_TIMEOUT_POLL_PERIOD_DIVISOR 30
/* [한국어] timeout_sec 에서 폴러 주기를 유도할 때 사용하는 분모. 예: timeout=30s →
 * 폴러 주기 = 30*1000000us / 30 = 1초. 즉 타임아웃 대비 충분히 자주 검사. */

#define DEFAULT_INITIATOR_NAME "iqn.2016-06.io.spdk:init"
/* [한국어] 기본 initiator IQN — iSCSI Qualified Name 표준 형식. 사용자가 RPC 로
 * 별도 IQN 을 지정하지 않으면 이 값이 사용된다 (현 코드에서는 사용자 지정 IQN 필수). */

/* MAXIMUM UNMAP LBA COUNT:
 * indicates the maximum  number of LBAs that may be unmapped
 * by an UNMAP command.
 */
/* [한국어] target 의 INQUIRY Block Limits VPD 페이지에서 max_unmap 값이 0/없을 때
 * fall-back 으로 사용하는 기본 최대 UNMAP LBA 수 (32768 블록). 너무 큰 값으로 보내면
 * target 이 거절할 수 있으므로 안전한 기본값. */
#define BDEV_ISCSI_DEFAULT_MAX_UNMAP_LBA_COUNT (32768)

/* MAXIMUM UNMAP BLOCK DESCRIPTOR COUNT:
 * indicates the maximum number of UNMAP block descriptors that
 * shall be contained in the parameter data transferred to the
 * device server for an UNMAP command.
 */
/* [한국어] UNMAP 파라미터 데이터에 담을 수 있는 block descriptor 개수 한계.
 * SCSI UNMAP 은 여러 [lba, num_blocks] 범위를 한 번에 전달 가능하지만, 본 모듈은
 * 단순화를 위해 1개만 사용한다 (= bdev 한 IO 당 한 범위). */
#define BDEV_ISCSI_MAX_UNMAP_BLOCK_DESCS_COUNT (1)

static int bdev_iscsi_initialize(void);
/* [한국어] 모듈 init 함수 forward decl — spdk_bdev_module 테이블에서 참조. */
static void bdev_iscsi_readcapacity16(struct iscsi_context *context, struct bdev_iscsi_lun *lun);
/* [한국어] CAPACITY_DATA_HAS_CHANGED sense 수신 시 재발행하는 READ_CAPACITY_16 forward decl. */
static void _bdev_iscsi_submit_request(void *_bdev_io);
/* [한국어] 실제 main_td 컨텍스트에서 실행되는 submit_request 본체 forward decl —
 * cross-thread send_msg 의 콜백으로 사용되므로 미리 선언. */

static TAILQ_HEAD(, bdev_iscsi_conn_req) g_iscsi_conn_req = TAILQ_HEAD_INITIALIZER(
			g_iscsi_conn_req);
/* [한국어] 진행 중인 모든 iSCSI 연결 요청을 추적하는 글로벌 큐. login phase 동안
 * libiscsi 컨텍스트는 main_td 에 바인딩되기 전이므로 별도 폴러(g_conn_poller)에서
 * 일괄 polling 한다. 큐가 비면 폴러가 자기 자신을 unregister. */
static struct spdk_poller *g_conn_poller = NULL;
/* [한국어] 위 큐에 대한 단일 connection 폴러. 첫 요청이 등록될 때 lazy 하게 생성,
 * 큐가 비면 해제. iscsi_bdev_conn_poll() 이 콜백. */

/* [한국어] per-IO 컨텍스트 구조체. spdk_bdev_io 의 driver_ctx 영역에 in-place 로
 * 임베드된다 (bdev_iscsi_get_ctx_size 가 sizeof 를 반환). bdev_io 가 살아있는
 * 동안에만 유효하며, 완료 후 free 는 bdev 코어가 처리. */
struct bdev_iscsi_io {
	struct spdk_thread *submit_td;
	/* [한국어] 이 I/O 를 처음 제출한 SPDK 스레드. main_td (LUN 의 polling 스레드) 와
	 * 다르면 spdk_thread_send_msg 로 main_td 에 위임하고, 완료 시점에도 다시
	 * submit_td 로 send_msg 하여 application 이 요청한 스레드에서 콜백을 실행한다.
	 * 설정자: bdev_iscsi_submit_request (submit_td != main_td 일 때만 non-NULL).
	 * 읽는 자: _bdev_iscsi_io_complete 직전, bdev_iscsi_io_complete 가 분기 결정.
	 * 값 범위: 유효 spdk_thread* 또는 NULL (= 동일 스레드).
	 * 동기화: 단일 IO 단위라 race 없음. */

	struct bdev_iscsi_lun *lun;
	/* [한국어] 이 IO 가 향하는 LUN 의 백 포인터. command_cb 에서 LUN 컨텍스트(특히
	 * resize 처리) 에 접근할 때 사용.
	 * 설정자: bdev_iscsi_submit_request 에서 iscsi_io->lun = lun.
	 * 읽는 자: bdev_iscsi_command_cb (CAPACITY_DATA_HAS_CHANGED 처리 경로 등).
	 * 값 범위: 유효 LUN 포인터 — IO 가 살아있는 동안 LUN 은 destruct 되지 않음
	 * (bdev 코어가 outstanding IO 가 있는 한 destruct 를 보류). */

	enum spdk_bdev_io_status status;
	/* [한국어] 최종 bdev 결과 상태 (SUCCESS / FAILED / NOMEM 등). _bdev_iscsi_io_complete
	 * 가 SUCCESS 이면 spdk_bdev_io_complete_scsi_status (sense 포함) 를 호출하고,
	 * 다른 값이면 spdk_bdev_io_complete 를 호출한다.
	 * 설정자: bdev_iscsi_io_complete().
	 * 읽는 자: _bdev_iscsi_io_complete(). */

	int scsi_status;
	/* [한국어] SCSI status code (GOOD/CHECK_CONDITION/RESERVATION_CONFLICT 등).
	 * spdk_bdev_io_complete_scsi_status 의 인자로 전달되어 상위 application 에
	 * SCSI 레벨 상태를 알린다.
	 * 설정자: bdev_iscsi_command_cb (libiscsi 가 알려준 status).
	 * 읽는 자: _bdev_iscsi_io_complete. */

	enum spdk_scsi_sense sk;
	/* [한국어] Sense Key (NO_SENSE/RECOVERED_ERROR/NOT_READY/MEDIUM_ERROR/...).
	 * task->sense.key 값 캐스팅. 상위에서 에러 분류에 사용. */

	uint8_t asc;
	/* [한국어] Additional Sense Code — sense data 의 상위 바이트.
	 * task->sense.ascq 가 (ASC<<8 | ASCQ) 로 인코딩되어 있어 (>>8 & 0xFF) 로 추출. */

	uint8_t ascq;
	/* [한국어] Additional Sense Code Qualifier — sense data 의 하위 바이트.
	 * 예: 0x29 (ASC=POWER ON, RESET, OR BUS DEVICE RESET OCCURRED). */
};

/* [한국어] LUN 1개당 1개 생성되는 메인 컨텍스트. spdk_bdev 를 임베드하여 bdev
 * 코어가 ctx 포인터로 LUN 전체를 추적 가능 (bdev->ctxt = lun). 또한 io_device
 * 로도 등록되어 채널 생성 시 spdk_io_channel 에서 lun 을 lookup. */
struct bdev_iscsi_lun {
	struct spdk_bdev		bdev;
	/* [한국어] 상위 bdev 코어가 사용하는 표준 bdev 디스크립터.
	 * blocklen, blockcnt, max_unmap, fn_table 등이 채워진다. ctx 는 lun 자신.
	 * 설정자: create_iscsi_lun() 에서 READ_CAPACITY 결과로 채움.
	 * 읽는 자: bdev 코어, fn_table 콜백들. */

	struct iscsi_context		*context;
	/* [한국어] libiscsi 의 세션 컨텍스트. TCP 소켓 fd, login 상태, 송수신 큐 등
	 * libiscsi 가 모두 캡슐화. iscsi_create_context 로 할당, iscsi_destroy_context
	 * 로 해제. 본 모듈은 fd 를 iscsi_get_fd 로만 노출 받아 poll(2) 에 사용.
	 * 동기화: main_td 에서만 호출되도록 thread affinity 유지 (submit_request 가
	 * cross-thread 호출이면 send_msg 로 main_td 에 위임). */

	char				*initiator_iqn;
	/* [한국어] 본 initiator 의 IQN 문자열 (예: "iqn.2016-06.io.spdk:init").
	 * dump_info_json / write_config_json 에 사용. strdup 으로 할당, _iscsi_free_lun 에서 free. */

	int				lun_id;
	/* [한국어] iSCSI 세션 내 LUN 번호. 같은 target 에 여러 LUN 이 있을 수 있고
	 * URL 파싱 결과에서 얻는다. SCSI CDB 의 LUN 필드와 libiscsi task 함수의
	 * lun 파라미터로 전달. */

	char				*url;
	/* [한국어] iSCSI URL 원본 문자열 (예: "iscsi://user:pwd@host/iqn.../0").
	 * dump_info_json 에 사용. iscsi_parse_full_url() 입력으로도 사용. */

	pthread_mutex_t			mutex;
	/* [한국어] ch_count / main_td 의 race-free 갱신을 위한 POSIX 뮤텍스. 채널의
	 * 생성/파괴가 다른 스레드들에서 동시에 일어날 수 있어 필요. SPDK 가 일반적으로
	 * lockless 를 추구하지만, libiscsi 는 스레드 단일 점유를 요구하므로 LUN 자체의
	 * 핸드오프 시점에 한해 mutex 사용. */

	uint32_t			ch_count;
	/* [한국어] 현재 살아있는 io_channel 개수. 첫 채널이 생성되면 main_td 와 poller
	 * 가 초기화되고, 마지막 채널이 파괴되면 poller 가 unregister 된다 (LUN 은 유지).
	 * mutex 로 보호. */

	struct spdk_thread		*main_td;
	/* [한국어] 이 LUN 의 libiscsi context 를 폴링/사용하는 "오너 스레드". 첫 채널이
	 * 생성된 스레드가 main_td 가 되며, 모든 iscsi_* API 호출은 이 스레드에서만
	 * 일어나야 안전하다. 다른 스레드에서 들어온 submit_request 는 spdk_thread_send_msg
	 * 로 main_td 에 위임. */

	struct spdk_poller		*no_main_ch_poller;
	/* [한국어] main_td 가 아직 정해지지 않았거나 채널이 0 인 동안 background 에서
	 * libiscsi 응답을 처리하기 위한 폴러. lun 생성 직후 등록되어 평생 살아있고,
	 * 채널이 생기면 idle 로 동작 (ch_count > 0 인 동안 polling 생략). */

	struct spdk_thread		*no_main_ch_poller_td;
	/* [한국어] no_main_ch_poller 가 등록된 스레드. destruct_cb 가 이 스레드에서
	 * 폴러를 unregister 하기 위해 저장 (spdk_thread_send_msg 의 destination). */

	bool				unmap_supported;
	/* [한국어] target 이 logical block provisioning (UNMAP) 을 지원하는지 여부.
	 * INQUIRY (LBP VPD page) 의 lbpu 비트로 결정. true 면 io_type_supported() 가
	 * SPDK_BDEV_IO_TYPE_UNMAP 에 true 반환. */

	uint32_t			max_unmap;
	/* [한국어] target 이 보고한 한 번에 UNMAP 가능한 최대 LBA 수.
	 * INQUIRY Block Limits VPD page 의 max_unmap. bdev_iscsi_unmap() 에서 큰
	 * UNMAP 요청을 이 값 단위로 분할(현 코드는 BDEV_ISCSI_MAX_UNMAP_BLOCK_DESCS_COUNT=1
	 * 이라 실제 분할은 미사용, 초과 시 에러). */

	struct spdk_poller		*poller;
	/* [한국어] main_td 의 메인 I/O 폴러. bdev_iscsi_poll_lun 을 0us 주기 (즉 매 reactor
	 * 루프) 로 호출. ch_count 0 → poller unregister, ch_count 0 → 1 시 재등록. */

	struct spdk_poller		*timeout_poller;
	/* [한국어] libiscsi 의 자체 timeout 체크용 폴러. iscsi_service(ctx, 0) 만 호출 →
	 * libiscsi 가 outstanding task 의 expiry 검사 후 응답 없으면 task 에 timeout
	 * 콜백 발동. g_opts.timeout_sec > 0 인 경우에만 등록. */
};

/* [한국어] per-channel 컨텍스트. bdev 코어가 spdk_io_channel 의 ctx_buf 로 할당.
 * 본 모듈은 채널 단위 자원이 거의 없어 LUN 백포인터만 보관 (대신 LUN 의 main_td
 * 가 모든 일을 한다). */
struct bdev_iscsi_io_channel {
	struct bdev_iscsi_lun	*lun;
	/* [한국어] 이 채널이 속한 LUN. create_cb 에서 ch->lun = lun 으로 설정. */
};

/* [한국어] 비동기 connect (login phase) 진행 중인 요청 1건의 상태. login 이 끝나면
 * INQUIRY → READ_CAPACITY → create_iscsi_lun 순서로 진행되어 최종 bdev 생성.
 * 진행 중인 모든 요청은 g_iscsi_conn_req 글로벌 큐에 들어간다. */
struct bdev_iscsi_conn_req {
	char					*url;
	/* [한국어] 사용자가 RPC 로 전달한 iSCSI URL. create_iscsi_lun 에 그대로 양도
	 * (lun->url 로 이전)되며 conn_req free 시에는 이미 lun 소유여서 free 안 함. */

	char					*bdev_name;
	/* [한국어] 생성할 bdev 이름. 마찬가지로 lun->bdev.name 으로 ownership 양도. */

	char					*initiator_iqn;
	/* [한국어] initiator IQN. ownership 은 lun 으로 이전. */

	struct iscsi_context			*context;
	/* [한국어] libiscsi 컨텍스트 — iscsi_create_context 로 할당. 성공 시 lun 으로
	 * ownership 이전, 실패 시 _bdev_iscsi_conn_req_free 가 destroy. */

	spdk_bdev_iscsi_create_cb		create_cb;
	/* [한국어] 완료 콜백 — 보통 RPC 응답 작성 함수 (bdev_iscsi_rpc.c 의
	 * bdev_iscsi_create_cb). create 성공/실패 둘 다에서 호출됨. */

	void					*create_cb_arg;
	/* [한국어] create_cb 의 user_data. 보통 spdk_jsonrpc_request*. */

	bool					unmap_supported;
	/* [한국어] INQUIRY LBP 결과 캐시 — login 후 단계별로 채워서 create_iscsi_lun
	 * 시점에 lun 에 옮긴다. */

	uint32_t				max_unmap;
	/* [한국어] INQUIRY Block Limits 결과 캐시 — 마찬가지로 lun 에 이전. */

	int					lun;
	/* [한국어] URL 에서 파싱한 LUN 번호. iscsi_full_connect_async() 입력으로 사용. */

	int					status;
	/* [한국어] 연결 진행 상태:
	 *   = 0 (SCSI_STATUS_GOOD): 아직 진행 중 또는 정상 완료
	 *   > 0: target 에서 받은 SCSI status (오류)
	 *   < 0 (-1): create_iscsi_disk 가 큐에 넣은 직후 상태 — login 진행 중
	 * iscsi_bdev_conn_poll() 이 != 0 이면 req 를 해제. */

	TAILQ_ENTRY(bdev_iscsi_conn_req)	link;
	/* [한국어] g_iscsi_conn_req 큐 연결. */
};

/* [한국어] 모듈 전역 옵션. timeout_sec 와 폴러 주기를 RPC bdev_iscsi_set_options
 * 가 갱신할 수 있다. 기본은 30초 timeout, 1초 주기 timeout 폴러. */
static struct spdk_bdev_iscsi_opts g_opts = {
	.timeout_sec = BDEV_ISCSI_TIMEOUT_DEFAULT,                       /* [한국어] 30초 — 개별 SCSI 명령 응답 대기. */
	.timeout_poller_period_us = BDEV_ISCSI_TIMEOUT_POLL_PERIOD_DEFAULT, /* [한국어] 1초 — timeout 검사 폴러 주기. */
};

/*
 * [한국어]
 * bdev_iscsi_get_opts - 현재 전역 옵션을 외부에 복사하여 반환.
 *
 * @opts: 호출자가 미리 할당한 spdk_bdev_iscsi_opts 버퍼. *opts 에 g_opts 복사.
 *
 * RPC 핸들러(bdev_iscsi_rpc.c 의 rpc_bdev_iscsi_set_options)가 "현재 값을 기반으로
 * decode 시작" 패턴(필수가 아닌 필드 보존)을 위해 호출한다.
 * 실행 컨텍스트: RPC 처리 스레드 (보통 첫 reactor) — 단순 복사라 동기화 불필요.
 */
void
bdev_iscsi_get_opts(struct spdk_bdev_iscsi_opts *opts)
{
	*opts = g_opts;     /* [한국어] 구조체 단위 복사 — 호출자가 자유롭게 수정해도 g_opts 에 영향 없음. */
}

/*
 * [한국어]
 * bdev_iscsi_set_opts - 전역 옵션을 갱신. timeout_poller 주기는 timeout_sec 에서 자동 유도.
 *
 * @opts: 새로 적용할 옵션. timeout_sec 가 핵심이며, poller_period_us 는 본 함수에서
 *        timeout_sec * 1000000 / 30 으로 덮어쓴다.
 * @return: 0 (현재 항상 성공). 향후 검증 추가 여지.
 *
 * RPC bdev_iscsi_set_options 가 호출. 변경된 g_opts 는 이후 새로 생성되는 LUN 부터만
 * 적용됨 (기존 timeout_poller 는 등록 시점의 주기 유지).
 * 호출 체인: rpc_bdev_iscsi_set_options → [이 함수].
 */
int
bdev_iscsi_set_opts(struct spdk_bdev_iscsi_opts *opts)
{
	/* make the poller period equal to timeout / 30 */
	/* [한국어] 타임아웃 검사 폴러를 timeout_sec / 30 주기로 설정 — timeout 까지 최대
	 * 30번 검사 기회 보장. 1초 주기는 일반적인 SPDK 폴러 대비 매우 느려 CPU 낭비 없음. */
	opts->timeout_poller_period_us = (opts->timeout_sec * 1000000ULL) /
					 BDEV_ISCSI_TIMEOUT_POLL_PERIOD_DIVISOR;

	g_opts = *opts;     /* [한국어] 전역에 반영 — 이후 SPDK_POLLER_REGISTER 들이 새 주기 사용. */

	return 0;           /* [한국어] 현재는 항상 성공. */
}

/*
 * [한국어]
 * complete_conn_req - 비동기 connect 요청 1건을 종료. RPC 응답 콜백 실행 + 큐에서 제거.
 *
 * @req: 종료할 conn_req. 이미 g_iscsi_conn_req 큐에 있어야 함.
 * @bdev: 생성된 bdev 포인터 (성공 시), 실패 시 NULL.
 * @status: 0=성공, !=0=실패 (SCSI status 또는 음수 errno).
 *
 * 주의: 이 함수는 iscsi_service() 내부 콜백 컨텍스트에서 실행된다 — libiscsi 의
 * 내부 자료구조가 아직 unwound 되지 않았으므로 iscsi_destroy_context() 같은 파괴는
 * 안전하지 않다. 따라서 status 만 기록해두고, 실제 해제는 다음 폴러 tick 에서
 * iscsi_bdev_conn_poll() 이 수행한다 (req->status > 0 → _bdev_iscsi_conn_req_free).
 */
static void
complete_conn_req(struct bdev_iscsi_conn_req *req, struct spdk_bdev *bdev,
		  int status)
{
	TAILQ_REMOVE(&g_iscsi_conn_req, req, link);                /* [한국어] 큐에서 제거 — conn_poll 이 더 이상 이 req 를 안 봄. */
	req->create_cb(req->create_cb_arg, bdev, status);          /* [한국어] RPC 응답 콜백 호출 — 사용자에게 성공/실패 통지. */

	/*
	 * we are still running in the context of iscsi_service()
	 * so do not tear down its data structures here
	 */
	/* [한국어] 위 영문 주석대로 — req 의 실제 free 는 다음 conn_poll() 의 status > 0
	 * 또는 status == 0 분기에서 처리. 여기서는 상태만 기록. */
	req->status = status;                                      /* [한국어] 0=성공, >0=실패(SCSI status) — conn_poll 의 분기 키. */
}

/*
 * [한국어]
 * bdev_iscsi_get_ctx_size - bdev_io 의 driver_ctx 영역에 임베드될 per-IO 컨텍스트 크기 보고.
 *
 * @return: sizeof(struct bdev_iscsi_io).
 *
 * bdev 코어가 모듈 등록 시 한 번 호출하여, spdk_bdev_io 의 driver_ctx 슬롯 크기를
 * 결정한다. 이후 모든 bdev_io 는 헤더 뒤에 이 크기만큼 추가 메모리가 할당되어
 * driver_ctx 로 노출된다. 즉 별도 alloc 없이 per-IO 상태 보관 가능.
 */
static int
bdev_iscsi_get_ctx_size(void)
{
	return sizeof(struct bdev_iscsi_io);   /* [한국어] bdev_iscsi_io 의 정적 크기. */
}

/*
 * [한국어]
 * _iscsi_free_lun - LUN 의 모든 자원을 해제하고 bdev 코어에 destruct 완료 통지.
 *
 * @arg: 해제할 bdev_iscsi_lun*.
 *
 * spdk_io_device_unregister 의 unregister_cb 로 호출된다. 호출 시점에는 io_device
 * 의 모든 채널이 이미 파괴되었고 outstanding IO 도 없는 상태가 보장된다.
 * iscsi_destroy_context() 가 내부적으로 iscsi_disconnect() 도 수행하므로 명시적
 * disconnect 는 불필요. 마지막에 spdk_bdev_destruct_done(bdev, 0) 로 bdev 코어에
 * "이제 LUN 메모리 free 했다" 알린 뒤, LUN 메모리 자체를 free.
 */
static void
_iscsi_free_lun(void *arg)
{
	struct bdev_iscsi_lun *lun = arg;                  /* [한국어] void* 를 LUN 포인터로 캐스팅. */

	assert(lun != NULL);                               /* [한국어] 호출자가 NULL 보내면 안 됨 — 방어. */
	iscsi_destroy_context(lun->context);               /* [한국어] libiscsi 세션 종료 + 컨텍스트 free. */
	pthread_mutex_destroy(&lun->mutex);                /* [한국어] 채널 라이프사이클 보호용 뮤텍스 해제. */
	free(lun->bdev.name);                              /* [한국어] strdup 으로 할당된 bdev 이름. */
	free(lun->url);                                    /* [한국어] strdup 으로 할당된 URL. */
	free(lun->initiator_iqn);                          /* [한국어] strdup 으로 할당된 IQN. */

	spdk_bdev_destruct_done(&lun->bdev, 0);            /* [한국어] bdev 코어에 destruct 완료 + 결과 0(성공) 통지. */
	free(lun);                                         /* [한국어] LUN 본체 free — 임베드된 bdev 도 함께 해제됨. */
}

/*
 * [한국어]
 * _bdev_iscsi_conn_req_free - 연결 요청(conn_req) 의 자원 해제.
 *
 * @req: 해제할 요청. 이미 큐에서 제거되어 있어야 한다.
 *
 * 사용 시점:
 *  (1) bdev_iscsi_finish() — 모듈 종료 시 큐에 남은 모든 pending 요청 정리.
 *  (2) iscsi_bdev_conn_poll() 에서 req->status > 0 (실패) 일 때.
 *  (3) create_iscsi_disk() 의 에러 경로 (큐에 넣기 전 실패).
 * iscsi_destroy_context() 가 connected 상태면 iscsi_disconnect() 도 호출하므로
 * 별도 disconnect 불필요.
 */
static void
_bdev_iscsi_conn_req_free(struct bdev_iscsi_conn_req *req)
{
	free(req->initiator_iqn);                          /* [한국어] strdup 으로 할당된 IQN. */
	free(req->bdev_name);                              /* [한국어] strdup 으로 할당된 bdev 이름. */
	free(req->url);                                    /* [한국어] strdup 으로 할당된 URL. */
	/* destroy will call iscsi_disconnect() implicitly if connected */
	iscsi_destroy_context(req->context);               /* [한국어] libiscsi 컨텍스트 — 위 영문 주석대로 disconnect 도 처리. */
	free(req);                                         /* [한국어] req 본체 free. */
}

/*
 * [한국어]
 * bdev_iscsi_finish - 모듈 종료 콜백. pending 연결 요청 모두 정리 + conn_poller unregister.
 *
 * spdk_bdev_module.module_fini 로 등록되어 spdk_app_stop() 경로에서 호출된다.
 * bdev 코어가 모든 bdev 를 unregister 한 후 호출하므로 LUN 들은 이미 정리되어 있고,
 * 진행 중이던 connect 요청만 남아있을 수 있다.
 */
static void
bdev_iscsi_finish(void)
{
	struct bdev_iscsi_conn_req *req, *tmp;             /* [한국어] FOREACH_SAFE 용 cursor & next. */

	/* clear out pending connection requests here. We cannot
	 * simply set the state to a non SCSI_STATUS_GOOD state as
	 * the connection poller won't run anymore
	 */
	/* [한국어] 위 영문 주석: 그냥 status 를 fail 로 두면 conn_poller 가 더 이상 안
	 * 돌아가서 free 가 안 됨. 따라서 여기서 명시적으로 모두 free. */
	TAILQ_FOREACH_SAFE(req, &g_iscsi_conn_req, link, tmp) {
		_bdev_iscsi_conn_req_free(req);            /* [한국어] 각 req 정리 (libiscsi destroy 포함). */
	}

	if (g_conn_poller) {                               /* [한국어] poller 가 등록된 상태였다면. */
		spdk_poller_unregister(&g_conn_poller);    /* [한국어] reactor 에서 폴러 제거. */
	}
}

/*
 * [한국어]
 * bdev_iscsi_opts_config_json - 모듈 옵션을 JSON-RPC 명령 형태로 직렬화.
 *
 * @w: spdk_json_write_ctx — 출력 스트림.
 *
 * spdk_bdev_subsystem_config_json() 같은 함수가 현재 SPDK 설정을 JSON 으로
 * dump 할 때 호출 — 결과는 "bdev_iscsi_set_options" RPC 명령 형태로 재현 가능.
 * 즉 save_config → load_config 라운드트립을 지원.
 */
static void
bdev_iscsi_opts_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);                                   /* [한국어] 최상위 { 시작. */

	spdk_json_write_named_string(w, "method", "bdev_iscsi_set_options"); /* [한국어] RPC 메서드 이름. */

	spdk_json_write_named_object_begin(w, "params");                   /* [한국어] params: { 시작. */
	spdk_json_write_named_uint64(w, "timeout_sec", g_opts.timeout_sec); /* [한국어] timeout_sec 만 출력 — poller 주기는 유도값이라 생략. */
	spdk_json_write_object_end(w);                                     /* [한국어] params } 종료. */

	spdk_json_write_object_end(w);                                     /* [한국어] 최상위 } 종료. */
}

/*
 * [한국어]
 * bdev_iscsi_config_json - 모듈의 config_json 콜백. 옵션만 출력.
 *
 * @w: JSON writer.
 * @return: 0 (실패 케이스 없음).
 *
 * 개별 LUN 설정은 bdev_iscsi_write_config_json (bdev fn_table) 에서 출력하고,
 * 여기서는 글로벌 모듈 옵션만 다룬다.
 */
static int
bdev_iscsi_config_json(struct spdk_json_write_ctx *w)
{
	bdev_iscsi_opts_config_json(w);   /* [한국어] 위 헬퍼에 위임. */
	return 0;                         /* [한국어] 항상 성공. */
}

/* [한국어] 본 bdev 모듈 디스크립터 — bdev 코어가 이 vtable 로 모듈을 식별.
 * SPDK_BDEV_MODULE_REGISTER 매크로가 constructor 단계에서 bdev 코어 리스트에 추가. */
static struct spdk_bdev_module g_iscsi_bdev_module = {
	.name		= "iscsi",                       /* [한국어] 모듈 이름 — RPC 응답 / 로그에 표시. */
	.module_init	= bdev_iscsi_initialize,         /* [한국어] init 콜백 — 현재 noop. */
	.module_fini	= bdev_iscsi_finish,             /* [한국어] fini 콜백 — pending conn_req 정리. */
	.config_json	= bdev_iscsi_config_json,        /* [한국어] config_json 콜백 — set_options 재현. */
	.get_ctx_size	= bdev_iscsi_get_ctx_size,       /* [한국어] driver_ctx 크기 — bdev_iscsi_io 만큼. */
};

SPDK_BDEV_MODULE_REGISTER(iscsi, &g_iscsi_bdev_module);
/* [한국어] 매크로 — constructor 우선순위(__attribute__((constructor))) 로 위 vtable 을
 * bdev 코어의 모듈 리스트에 등록한다. "iscsi" 이름은 RPC bdev_get_bdevs 결과 등에 노출. */

/*
 * [한국어]
 * _bdev_iscsi_io_complete - bdev_io 완료의 실제 실행자 (반드시 submit_td 컨텍스트에서).
 *
 * @_iscsi_io: spdk_bdev_io 의 driver_ctx 영역에 임베드된 bdev_iscsi_io*.
 *
 * iscsi_io->status 가 SUCCESS 이면 sense 정보까지 포함한 spdk_bdev_io_complete_scsi_status
 * 를, FAILED/NOMEM 이면 일반 spdk_bdev_io_complete 를 호출한다. 후자는 sense 가
 * 무의미한 경로 (예: submit 자체 실패).
 *
 * 실행 컨텍스트: 항상 submit_td (제출 스레드). main_td 가 다른 경우 호출자가
 * spdk_thread_send_msg 로 이 함수를 submit_td 에 dispatch.
 */
static void
_bdev_iscsi_io_complete(void *_iscsi_io)
{
	struct bdev_iscsi_io *iscsi_io = _iscsi_io;        /* [한국어] void* 캐스팅. */

	if (iscsi_io->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		/* [한국어] SCSI status + sense data 를 함께 보고. bdev 코어는 이를 받아
		 * application 의 cb 에서 spdk_bdev_io 의 fields 로 노출. */
		spdk_bdev_io_complete_scsi_status(spdk_bdev_io_from_ctx(iscsi_io), iscsi_io->scsi_status,
						  iscsi_io->sk, iscsi_io->asc, iscsi_io->ascq);
	} else {
		/* [한국어] 일반 실패 경로 — sense 정보 없이 status (FAILED/NOMEM) 만 보고. */
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(iscsi_io), iscsi_io->status);
	}
}

/*
 * [한국어]
 * bdev_iscsi_io_complete - 완료를 submit_td 컨텍스트로 dispatch.
 *
 * @iscsi_io: 완료시킬 IO 컨텍스트.
 * @status: 최종 bdev 상태 (SUCCESS / FAILED / NOMEM).
 *
 * libiscsi 콜백은 main_td 에서 실행되지만 IO 를 제출한 스레드는 다를 수 있으므로,
 * 제출 스레드(submit_td)가 별도로 있으면 spdk_thread_send_msg 로 위임한다. 이는
 * bdev 코어가 "완료 콜백은 제출 스레드에서 실행" 규약을 요구하기 때문.
 */
static void
bdev_iscsi_io_complete(struct bdev_iscsi_io *iscsi_io, enum spdk_bdev_io_status status)
{
	iscsi_io->status = status;                         /* [한국어] 결과 status 저장 — 곧 _complete 가 읽음. */
	if (iscsi_io->submit_td != NULL) {                 /* [한국어] cross-thread 제출이었다면. */
		/* [한국어] submit_td 로 메시지 전달 — main_td 와 다른 reactor 일 수 있으므로
		 * lockless 메시지 큐(spdk_thread send_msg)를 사용. */
		spdk_thread_send_msg(iscsi_io->submit_td, _bdev_iscsi_io_complete, iscsi_io);
	} else {                                           /* [한국어] 같은 스레드에서 제출된 IO. */
		_bdev_iscsi_io_complete(iscsi_io);         /* [한국어] 직접 즉시 호출. */
	}
}

/*
 * [한국어]
 * _bdev_iscsi_is_size_change - sense data 가 "용량 변경됨" 알림인지 검사.
 *
 * @status: SCSI status (CHECK_CONDITION 인지).
 * @task: libiscsi task — sense key/ascq 검사.
 * @return: true 면 target LUN 의 용량이 변경됨 — READ_CAPACITY 재발행 필요.
 *
 * iSCSI target 이 동적으로 LUN 크기를 변경하면 (예: thin-provision 확장) 다음 명령에
 * UNIT_ATTENTION + ASCQ 0x2a09 (CAPACITY DATA HAS CHANGED) 가 부착되어 응답된다.
 * 본 모듈은 이를 감지해 READ_CAPACITY_16 으로 새 크기를 가져오고 spdk_bdev 의
 * blockcnt 를 갱신(spdk_bdev_notify_blockcnt_change).
 */
static bool
_bdev_iscsi_is_size_change(int status, struct scsi_task *task)
{
	if (status == SPDK_SCSI_STATUS_CHECK_CONDITION &&                   /* [한국어] sense data 가 유효한 응답. */
	    (uint8_t)task->sense.key == SPDK_SCSI_SENSE_UNIT_ATTENTION &&   /* [한국어] Sense Key = 0x6 — 비동기 상태 변경 알림. */
	    task->sense.ascq == 0x2a09) {                                   /* [한국어] ASC=0x2A ASCQ=0x09 — CAPACITY DATA HAS CHANGED. */
		/* ASCQ: SCSI_SENSE_ASCQ_CAPACITY_DATA_HAS_CHANGED (0x2a09) */
		return true;                                                /* [한국어] 용량 변경됨 — 재발행 필요. */
	}

	return false;                                                       /* [한국어] 그 외 sense 는 일반 오류로 처리. */
}

/* Common call back function for read/write/flush command */
/*
 * [한국어]
 * bdev_iscsi_command_cb - READ16/WRITE16/SYNCHRONIZE_CACHE16/UNMAP 의 공통 완료 콜백.
 *
 * @context: libiscsi 세션 컨텍스트 (요청을 보낸 컨텍스트).
 * @status: SCSI status (0=GOOD, !=0=오류).
 * @_task: 완료된 scsi_task* — sense data 가 들어있음.
 * @_iscsi_io: 우리가 queue 시점에 넘긴 user_data = bdev_iscsi_io*.
 *
 * 처리 흐름:
 *   1. status / sense 정보를 iscsi_io 에 캐싱 → 완료 시 상위에 전달.
 *   2. CAPACITY_DATA_HAS_CHANGED 라면 READ_CAPACITY 재발행 후 같은 bdev_io 를
 *      즉시 재제출 (즉 사용자에게는 실패 안 보이고 자동 재시도).
 *   3. 그 외에는 SCSI status 와 무관하게 (CHECK_CONDITION 도) SUCCESS 로 보고 —
 *      sense data 가 별도로 전달되므로 상위가 분류 가능.
 *   4. scsi_task 해제 (libiscsi 가 alloc, 호출자가 free 책임).
 *
 * 실행 컨텍스트: main_td — iscsi_service() 가 응답을 파싱한 후 직접 호출.
 */
static void
bdev_iscsi_command_cb(struct iscsi_context *context, int status, void *_task, void *_iscsi_io)
{
	struct scsi_task *task = _task;                    /* [한국어] void* → scsi_task 캐스팅. */
	struct bdev_iscsi_io *iscsi_io = _iscsi_io;        /* [한국어] void* → bdev_iscsi_io. */
	struct spdk_bdev_io *bdev_io;                      /* [한국어] 재시도 경로에서 사용. */

	iscsi_io->scsi_status = status;                    /* [한국어] SCSI status code (GOOD/CHECK_CONDITION 등). */
	iscsi_io->sk = (uint8_t)task->sense.key;           /* [한국어] Sense Key — 상위 4비트 카테고리. */
	iscsi_io->asc = (task->sense.ascq >> 8) & 0xFF;    /* [한국어] ASC — task->sense.ascq 의 상위 바이트. */
	iscsi_io->ascq = task->sense.ascq & 0xFF;          /* [한국어] ASCQ — 하위 바이트. */

	if (_bdev_iscsi_is_size_change(status, task)) {    /* [한국어] 용량 변경 알림이라면. */
		bdev_iscsi_readcapacity16(context, iscsi_io->lun); /* [한국어] 새 용량 비동기 조회 — blockcnt 갱신. */

		/* Retry this failed IO immediately */
		/* [한국어] 위 영문 주석: 실패한 IO 를 즉시 재시도 — 사용자 관점에서는 일시적인 hiccup 만 발생. */
		bdev_io = spdk_bdev_io_from_ctx(iscsi_io);     /* [한국어] driver_ctx → bdev_io 역참조. */
		if (iscsi_io->submit_td != NULL) {             /* [한국어] cross-thread 였다면 main_td 로 다시 전달. */
			spdk_thread_send_msg(iscsi_io->lun->main_td,
					     _bdev_iscsi_submit_request, bdev_io);
		} else {                                        /* [한국어] 같은 스레드 → 즉시 재제출. */
			_bdev_iscsi_submit_request(bdev_io);
		}
	} else {
		/* [한국어] 일반 완료 (오류 포함) — sense data 로 상위가 판단. SUCCESS 상태로
		 * 보고하는 이유는 spdk_bdev_io_complete_scsi_status 에 가는 경로를 타기 위해서. */
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}

	scsi_free_scsi_task(task);                         /* [한국어] libiscsi 가 alloc 한 task 해제 책임은 호출자. */
}

/*
 * [한국어]
 * bdev_iscsi_resize - bdev 코어에 blockcnt 변경 통지 (online resize).
 *
 * @bdev: 대상 bdev.
 * @new_size_in_block: 새 블록 수 (현재보다 커야 함).
 * @return: 0=성공, -EINVAL=축소 시도, 기타 음수=spdk_bdev_notify 실패.
 *
 * SPDK 는 online expand 만 지원 (축소는 데이터 유실 위험). 성공하면 상위 application
 * 이 등록한 size-change 이벤트가 발생한다.
 */
static int
bdev_iscsi_resize(struct spdk_bdev *bdev, const uint64_t new_size_in_block)
{
	int rc;

	assert(bdev->module == &g_iscsi_bdev_module);      /* [한국어] 자기 모듈 소속인지 — 잘못된 bdev 보호. */

	if (new_size_in_block <= bdev->blockcnt) {         /* [한국어] 같거나 작은 크기는 거절 — online shrink 미지원. */
		SPDK_ERRLOG("The new bdev size must be larger than current bdev size.\n");
		return -EINVAL;
	}

	rc = spdk_bdev_notify_blockcnt_change(bdev, new_size_in_block); /* [한국어] bdev 코어 API — 등록된 listener 에 이벤트 발송. */
	if (rc != 0) {
		SPDK_ERRLOG("failed to notify block cnt change.\n");
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * bdev_iscsi_readcapacity16_cb - READ_CAPACITY_16 응답 콜백 (online resize 경로).
 *
 * @context: 요청 보낸 libiscsi 컨텍스트.
 * @status: SCSI status.
 * @_task: 응답 task — scsi_datain_unmarshall 로 readcapacity16 구조체 파싱.
 * @private_data: bdev_iscsi_readcapacity16() 가 넘긴 LUN 포인터.
 *
 * 응답 LBA + 1 = 총 블록 수. bdev_iscsi_resize 로 bdev 코어에 통지.
 * 이 콜백은 초기 connect 경로가 아닌 "이미 운영 중인 LUN 의 사이즈 변경 응답" 용.
 * 초기 capacity 조회는 iscsi_readcapacity16_cb (다른 함수)가 처리.
 */
static void
bdev_iscsi_readcapacity16_cb(struct iscsi_context *context, int status, void *_task,
			     void *private_data)
{
	struct bdev_iscsi_lun *lun = private_data;         /* [한국어] private_data 는 LUN. */
	struct scsi_readcapacity16 *readcap16;             /* [한국어] 파싱된 결과 구조체 포인터. */
	struct scsi_task *task = _task;                    /* [한국어] task 본체. */
	uint64_t size_in_block = 0;                        /* [한국어] 새 블록 수. */
	int rc;

	if (status != SPDK_SCSI_STATUS_GOOD) {              /* [한국어] CHECK_CONDITION/RESERVATION_CONFLICT 등. */
		SPDK_ERRLOG("iSCSI error: %s\n", iscsi_get_error(context));
		goto ret;                                   /* [한국어] task free 만 하고 끝. */
	}

	readcap16 = scsi_datain_unmarshall(task);          /* [한국어] data-in buffer 를 구조체로 변환 (libiscsi 헬퍼). */
	if (!readcap16) {                                  /* [한국어] 파싱 실패 — 응답 데이터 손상 등. */
		SPDK_ERRLOG("Read capacity error\n");
		goto ret;
	}

	size_in_block = readcap16->returned_lba + 1;       /* [한국어] returned_lba 는 마지막 유효 LBA → +1 = 총 블록 수. */

	rc = bdev_iscsi_resize(&lun->bdev, size_in_block); /* [한국어] bdev 코어에 변경 알림. */
	if (rc != 0) {
		SPDK_ERRLOG("Bdev (%s) resize error: %d\n", lun->bdev.name, rc);
	}

ret:
	scsi_free_scsi_task(task);                         /* [한국어] task 메모리 해제 — 모든 경로에서 필요. */
}

/*
 * [한국어]
 * bdev_iscsi_readcapacity16 - 운영 중인 LUN 에 대해 READ_CAPACITY_16 명령 발행.
 *
 * @context: libiscsi 컨텍스트.
 * @lun: 대상 LUN — 콜백의 private_data 로 전달.
 *
 * CAPACITY_DATA_HAS_CHANGED sense 감지 시 호출되며, 응답이 도착하면
 * bdev_iscsi_readcapacity16_cb 에서 resize 수행. 비동기이므로 함수 자체는 즉시 리턴.
 */
static void
bdev_iscsi_readcapacity16(struct iscsi_context *context, struct bdev_iscsi_lun *lun)
{
	struct scsi_task *task;

	task = iscsi_readcapacity16_task(context, lun->lun_id,             /* [한국어] libiscsi: SCSI ServiceAction READ CAPACITY (16) CDB 빌드 + 전송 큐잉. */
					 bdev_iscsi_readcapacity16_cb, lun);
	if (task == NULL) {                                                 /* [한국어] alloc 실패 / 컨텍스트 비정상. */
		SPDK_ERRLOG("failed to get readcapacity16_task\n");
	}
}

/*
 * [한국어]
 * bdev_iscsi_readv - bdev READ 요청을 SCSI READ_16 명령으로 변환해 발행.
 *
 * @lun: 대상 LUN.
 * @iscsi_io: per-IO 컨텍스트.
 * @iov / @iovcnt: data-in 버퍼 (target → host).
 * @nbytes: 총 바이트 수 (num_blocks * blocklen).
 * @lba: 시작 LBA.
 *
 * libiscsi 의 iscsi_read16_task 가 READ(16) CDB (NVMe 의 READ 와 유사한 SCSI 명령)
 * 를 빌드해 TCP 로 송신 큐잉한다. 응답이 도착하면 bdev_iscsi_command_cb 가 호출되며
 * data-in 페이로드는 iov 에 직접 scatter 된다 (LIBISCSI_FEATURE_IOVECTOR 가 정의된
 * 경우 zero-copy 에 가까운 single-shot, 아니면 버퍼별 add 반복).
 *
 * 실행 컨텍스트: main_td. 호출자 (_bdev_iscsi_submit_request) 가 보장.
 */
static void
bdev_iscsi_readv(struct bdev_iscsi_lun *lun, struct bdev_iscsi_io *iscsi_io,
		 struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t lba)
{
	struct scsi_task *task;

	SPDK_DEBUGLOG(iscsi_init, "read %d iovs size %lu to lba: %#lx\n",
		      iovcnt, nbytes, lba);                  /* [한국어] 디버그 로그 — iscsi_init 컴포넌트 활성 시만 출력. */

	/* [한국어] libiscsi: SCSI READ(16) CDB.
	 *   opcode=0x88, lba/length 외에 추가 비트(reladdr=0, dpo=0, fua=0, fua_nv=0, rdprotect=0, group_number=0)
	 *   를 0 으로 — 본 모듈은 단순 READ 만 사용. blocklen 인자는 libiscsi 가 nbytes 와의 정합성 검증용. */
	task = iscsi_read16_task(lun->context, lun->lun_id, lba, nbytes, lun->bdev.blocklen, 0, 0, 0, 0, 0,
				 bdev_iscsi_command_cb, iscsi_io);
	if (task == NULL) {                                 /* [한국어] alloc/queue 실패 — 대부분 OOM 또는 컨텍스트 비정상. */
		SPDK_ERRLOG("failed to get read16_task\n");
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

#if defined(LIBISCSI_FEATURE_IOVECTOR)
	/* [한국어] libiscsi 가 iovec 인터페이스를 지원 — 단일 호출로 모든 iov 등록.
	 * scsi_iovec 와 iovec 는 같은 레이아웃이라 단순 cast 가능. */
	scsi_task_set_iov_in(task, (struct scsi_iovec *)iov, iovcnt);
#else
	/* [한국어] 구버전 libiscsi: 각 iov 를 add_data_in_buffer 로 하나씩 등록. */
	int i;
	for (i = 0; i < iovcnt; i++) {
		scsi_task_add_data_in_buffer(task, iov[i].iov_len, iov[i].iov_base);
	}
#endif
}

/*
 * [한국어]
 * bdev_iscsi_writev - bdev WRITE 요청을 SCSI WRITE_16 명령으로 변환.
 *
 * @lun: 대상 LUN.
 * @iscsi_io: per-IO 컨텍스트.
 * @iov / @iovcnt: data-out 버퍼 (host → target).
 * @nbytes: 총 바이트 수.
 * @lba: 시작 LBA.
 *
 * iscsi_write16_task 의 data 인자는 NULL — 대신 set_iov_out / add_data_in_buffer
 * 로 buffer 를 등록한다. WRITE(16) CDB opcode=0x8A. fua/dpo 등 부가 비트는 모두 0.
 */
static void
bdev_iscsi_writev(struct bdev_iscsi_lun *lun, struct bdev_iscsi_io *iscsi_io,
		  struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t lba)
{
	struct scsi_task *task;

	SPDK_DEBUGLOG(iscsi_init, "write %d iovs size %lu to lba: %#lx\n",
		      iovcnt, nbytes, lba);                  /* [한국어] 디버그 로그. */

	/* [한국어] libiscsi: SCSI WRITE(16) CDB. data=NULL → 이후 iov 등록. fua/dpo=0.
	 * blocklen 은 nbytes 와의 정합성 검증용. */
	task = iscsi_write16_task(lun->context, lun->lun_id, lba, NULL, nbytes, lun->bdev.blocklen, 0, 0, 0,
				  0, 0,
				  bdev_iscsi_command_cb, iscsi_io);
	if (task == NULL) {
		SPDK_ERRLOG("failed to get write16_task\n");
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

#if defined(LIBISCSI_FEATURE_IOVECTOR)
	scsi_task_set_iov_out(task, (struct scsi_iovec *)iov, iovcnt);   /* [한국어] data-out 측 iovec — host→target 방향. */
#else
	int i;
	for (i = 0; i < iovcnt; i++) {
		/* [한국어] NOTE: 원본 코드가 _in_ 함수를 사용 — libiscsi 구버전 호환 코드 경로.
		 * 실제로는 data-out 이지만 add_data_in_buffer 가 양방향 버퍼를 다룬다. */
		scsi_task_add_data_in_buffer(task, iov[i].iov_len, iov[i].iov_base);
	}
#endif
}

/*
 * [한국어]
 * bdev_iscsi_destruct_cb - destruct 두 번째 단계. background poller 제거 후 io_device 등록 해제.
 *
 * @ctx: LUN 포인터.
 *
 * spdk_thread_send_msg 로 no_main_ch_poller_td 에 dispatch 되어 실행. unregister 가
 * 완료되면 spdk_io_device_unregister 의 unregister_cb 인 _iscsi_free_lun 이
 * 비동기로 호출되어 최종 free 수행.
 */
static void
bdev_iscsi_destruct_cb(void *ctx)
{
	struct bdev_iscsi_lun *lun = ctx;

	spdk_poller_unregister(&lun->no_main_ch_poller);              /* [한국어] background polling 폴러 제거. */
	spdk_io_device_unregister(lun, _iscsi_free_lun);              /* [한국어] io_device 등록 해제 — 완료 시 _iscsi_free_lun 콜백. */
}

/*
 * [한국어]
 * bdev_iscsi_destruct - bdev fn_table.destruct 콜백. spdk_bdev_unregister 경로에서 호출.
 *
 * @ctx: LUN 포인터.
 * @return: 1 (비동기 destruct — 완료는 spdk_bdev_destruct_done 으로 추후 통지).
 *
 * destruct 는 no_main_ch_poller_td 에서 수행해야 안전하므로 send_msg 로 dispatch.
 * return 1 → bdev 코어는 이 모듈이 비동기 destruct 라고 인식하고, spdk_bdev_destruct_done
 * 호출을 기다린다 (_iscsi_free_lun 의 마지막에 호출).
 */
static int
bdev_iscsi_destruct(void *ctx)
{
	struct bdev_iscsi_lun *lun = ctx;

	assert(lun->no_main_ch_poller_td);                            /* [한국어] LUN 생성 시 set 되었어야 함. */
	spdk_thread_send_msg(lun->no_main_ch_poller_td, bdev_iscsi_destruct_cb, lun); /* [한국어] background 스레드로 dispatch. */
	return 1;                                                     /* [한국어] async destruct — bdev 코어에 알림. */
}

/*
 * [한국어]
 * bdev_iscsi_flush - bdev FLUSH 를 SCSI SYNCHRONIZE_CACHE(16) 으로 변환.
 *
 * @lun, @iscsi_io: 일반.
 * @num_blocks: flush 범위 (0 = entire LUN).
 * @immed: IMMED 비트 — 1 이면 target 이 즉시 status 응답하고 백그라운드 flush.
 *         본 모듈은 ISCSI_IMMEDIATE_DATA_NO 만 사용.
 * @lba: 시작 LBA.
 *
 * SCSI SYNCHRONIZE_CACHE(16) opcode=0x91. target 의 volatile write cache 를
 * non-volatile 매체로 flush. NVMe 의 FLUSH 와 유사.
 */
static void
bdev_iscsi_flush(struct bdev_iscsi_lun *lun, struct bdev_iscsi_io *iscsi_io, uint32_t num_blocks,
		 int immed, uint64_t lba)
{
	struct scsi_task *task;

	/* [한국어] libiscsi: SYNCHRONIZE_CACHE(16) CDB. 5번째 인자 sync_nv=0, immed=immed.
	 * num_blocks=0 이면 LBA 이후 전체 flush (SCSI 스펙). */
	task = iscsi_synchronizecache16_task(lun->context, lun->lun_id, lba,
					     num_blocks, 0, immed, bdev_iscsi_command_cb, iscsi_io);
	if (task == NULL) {
		SPDK_ERRLOG("failed to get sync16_task\n");
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
}

/*
 * [한국어]
 * bdev_iscsi_unmap - bdev UNMAP 을 SCSI UNMAP 명령으로 변환.
 *
 * @lun, @iscsi_io: 일반.
 * @lba: 해제 시작 LBA.
 * @num_blocks: 해제할 블록 수.
 *
 * SCSI UNMAP (opcode=0x42) 은 여러 [lba, num_blocks] 범위를 한 번에 전달 가능
 * (block descriptor list). 본 모듈은 BDEV_ISCSI_MAX_UNMAP_BLOCK_DESCS_COUNT=1 로
 * 제한하므로 lun->max_unmap 을 초과하는 큰 unmap 은 거절된다 (상위 bdev 코어가
 * max_unmap 을 알고 적절히 분할한 상태로 들어와야 함).
 */
static void
bdev_iscsi_unmap(struct bdev_iscsi_lun *lun, struct bdev_iscsi_io *iscsi_io,
		 uint64_t lba, uint64_t num_blocks)
{
	struct scsi_task *task;
	struct unmap_list list[BDEV_ISCSI_MAX_UNMAP_BLOCK_DESCS_COUNT] = {}; /* [한국어] descriptor list — 1개 슬롯. */
	struct unmap_list *entry;
	uint32_t num_unmap_list;
	uint64_t offset, remaining, unmap_blocks;

	/* [한국어] 요청된 num_blocks 를 max_unmap 으로 나눠 필요한 descriptor 수 계산.
	 * 1개 초과면 본 모듈에서는 다루지 못함 — 에러. */
	num_unmap_list = spdk_divide_round_up(num_blocks, lun->max_unmap);
	if (num_unmap_list > BDEV_ISCSI_MAX_UNMAP_BLOCK_DESCS_COUNT) {
		SPDK_ERRLOG("Too many unmap entries\n");
		goto failed;
	}

	remaining = num_blocks;                       /* [한국어] 남은 블록 수. */
	offset = lba;                                 /* [한국어] 다음 descriptor 의 시작 LBA. */
	num_unmap_list = 0;                           /* [한국어] 채워넣을 descriptor 수 카운터 재설정. */
	entry = &list[0];                             /* [한국어] 작성 cursor. */

	do {
		/* [한국어] 한 descriptor 의 최대 길이 = max_unmap (target 보고값). */
		unmap_blocks = spdk_min(remaining, lun->max_unmap);
		entry->lba = offset;                  /* [한국어] 시작 LBA. */
		entry->num = unmap_blocks;            /* [한국어] 길이. */
		num_unmap_list++;                     /* [한국어] descriptor 수 증가. */
		remaining -= unmap_blocks;            /* [한국어] 남은 블록 감소. */
		offset += unmap_blocks;               /* [한국어] 다음 시작 LBA 갱신. */
		entry++;                              /* [한국어] cursor 다음 슬롯으로. */
	} while (remaining > 0);

	/* [한국어] libiscsi: UNMAP CDB. 3번째/4번째 인자(anchor=0, group_number=0). */
	task = iscsi_unmap_task(lun->context, lun->lun_id, 0, 0, list, num_unmap_list,
				bdev_iscsi_command_cb, iscsi_io);
	if (task != NULL) {                           /* [한국어] 성공 — 완료는 콜백에서. */
		return;
	}
	SPDK_ERRLOG("failed to get unmap_task\n");

failed:
	bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED); /* [한국어] 모든 에러 경로 통합. */
}

/*
 * [한국어]
 * bdev_iscsi_reset_cb - iSCSI Task Management Function (LUN_RESET) 응답 콜백.
 *
 * @context: 컨텍스트 (사용 안 함).
 * @status: TMF 전송 자체의 status.
 * @command_data: uint32_t* — TMF 응답 코드.
 * @private_data: bdev_iscsi_io*.
 *
 * iSCSI TMF 응답이 ISCSI_TASK_FUNC_RESP_COMPLETE (=0) 면 reset 성공.
 * 그 외 (REJECTED, INCORRECT_LUN, ...) 는 실패로 처리.
 */
static void
bdev_iscsi_reset_cb(struct iscsi_context *context __attribute__((unused)), int status,
		    void *command_data, void *private_data)
{
	uint32_t tmf_response;
	struct bdev_iscsi_io *iscsi_io = private_data;

	tmf_response = *(uint32_t *)command_data;                /* [한국어] TMF response code (iSCSI 스펙 10.6.1). */
	if (tmf_response == ISCSI_TASK_FUNC_RESP_COMPLETE) {     /* [한국어] 0 = Function complete = 성공. */
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {                                                 /* [한국어] 1..7 = 다양한 실패 reason. */
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * [한국어]
 * _bdev_iscsi_reset - main_td 컨텍스트에서 실행되는 reset 본체.
 *
 * @_bdev_io: spdk_bdev_io*.
 *
 * libiscsi 의 LUN_RESET TMF 비동기 발행. SCSI command 가 아닌 iSCSI 프로토콜
 * 레벨의 task management function (스펙 10.6) 으로, target 에 "이 LUN 의 모든
 * outstanding command 를 abort 하라" 요청.
 */
static void
_bdev_iscsi_reset(void *_bdev_io)
{
	int rc;
	struct spdk_bdev_io *bdev_io = _bdev_io;
	struct bdev_iscsi_lun *lun = (struct bdev_iscsi_lun *)bdev_io->bdev->ctxt;     /* [한국어] bdev_io 에서 LUN 추출. */
	struct bdev_iscsi_io *iscsi_io = (struct bdev_iscsi_io *)bdev_io->driver_ctx;  /* [한국어] per-IO ctx. */
	struct iscsi_context *context = lun->context;                                  /* [한국어] libiscsi 세션. */

	rc = iscsi_task_mgmt_lun_reset_async(context, lun->lun_id,                     /* [한국어] LUN_RESET TMF 발행. */
					     bdev_iscsi_reset_cb, iscsi_io);
	if (rc != 0) {
		SPDK_ERRLOG("failed to do iscsi reset\n");
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
}

/*
 * [한국어]
 * bdev_iscsi_reset - bdev_iscsi_submit_request 의 RESET 분기. main_td 로 전달.
 *
 * @bdev_io: bdev IO.
 *
 * RESET 은 항상 main_td 에서 실행되어야 libiscsi 의 단일 스레드 가정을 위배하지 않음.
 */
static void
bdev_iscsi_reset(struct spdk_bdev_io *bdev_io)
{
	struct bdev_iscsi_lun *lun = (struct bdev_iscsi_lun *)bdev_io->bdev->ctxt;
	spdk_thread_send_msg(lun->main_td, _bdev_iscsi_reset, bdev_io);  /* [한국어] main_td 로 dispatch — 콜백은 _bdev_iscsi_reset. */
}

/*
 * [한국어]
 * bdev_iscsi_poll_lun - LUN 의 메인 I/O 폴러. libiscsi fd 를 poll(2) 후 iscsi_service 호출.
 *
 * @_lun: 폴링할 LUN.
 * @return: SPDK_POLLER_BUSY (실제 작업 했음) / SPDK_POLLER_IDLE (할 일 없음) —
 *          상위 reactor 가 통계용으로 사용.
 *
 * SPDK polled-mode 의 본체: timeout=0 인 non-blocking poll() 로 TCP 소켓 상태를
 * 검사하고, READ/WRITE 가능 이벤트가 있으면 iscsi_service() 가 송수신 처리.
 * 송수신 중 응답이 완성되면 libiscsi 가 등록된 콜백(bdev_iscsi_command_cb 등) 을
 * 동기적으로 호출.
 *
 * 실행 컨텍스트: main_td. 등록 시점에 main_td 가 결정됨 (bdev_iscsi_create_cb).
 */
static int
bdev_iscsi_poll_lun(void *_lun)
{
	struct bdev_iscsi_lun *lun = _lun;
	struct pollfd pfd = {};                          /* [한국어] poll(2) 인자 — events/revents 사용. */

	pfd.fd = iscsi_get_fd(lun->context);             /* [한국어] libiscsi 의 TCP 소켓 fd. */
	pfd.events = iscsi_which_events(lun->context);   /* [한국어] 현재 어떤 이벤트를 기다리는지 (POLLIN/POLLOUT). */

	if (poll(&pfd, 1, 0) < 0) {                      /* [한국어] non-blocking poll — timeout=0. */
		SPDK_ERRLOG("poll failed\n");
		return SPDK_POLLER_IDLE;
	}

	if (pfd.revents != 0) {                          /* [한국어] 처리 가능한 이벤트 발생. */
		/* [한국어] iscsi_service 가 실제 송수신 + 콜백 디스패치. 음수면 세션 오류. */
		if (iscsi_service(lun->context, pfd.revents) < 0) {
			SPDK_ERRLOG("iscsi_service failed: %s\n", iscsi_get_error(lun->context));
		}

		return SPDK_POLLER_BUSY;                  /* [한국어] 실제 일을 함 — reactor 가 통계 반영. */
	}

	return SPDK_POLLER_IDLE;                          /* [한국어] 이벤트 없음. */
}

/*
 * [한국어]
 * bdev_iscsi_poll_lun_timeout - libiscsi 의 task timeout 체크 전용 폴러.
 *
 * @_lun: LUN.
 * @return: SPDK_POLLER_BUSY (timeout 검사 자체는 실행했으므로).
 *
 * revents=0 으로 iscsi_service 호출 → 송수신은 안 하고 task timeout 만 체크.
 * timeout 된 task 가 있으면 libiscsi 가 등록된 콜백을 -ETIMEDOUT 으로 호출.
 */
static int
bdev_iscsi_poll_lun_timeout(void *_lun)
{
	struct bdev_iscsi_lun *lun = _lun;
	/* passing 0 here to iscsi_service means do nothing except for timeout checks */
	iscsi_service(lun->context, 0);                  /* [한국어] timeout 검사만 수행. */
	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * bdev_iscsi_no_main_ch_poll - 채널이 없는 동안 background polling.
 *
 * @arg: LUN.
 * @return: BUSY/IDLE.
 *
 * 채널이 모두 닫혀 main_td 가 없는 동안에도 unmap 같은 비동기 요청에 대한 응답
 * 도착 가능 — 누군가는 libiscsi fd 를 polling 해야 한다. 이 폴러가 그 역할을 하며,
 * 채널이 살아있을 때(ch_count > 0)는 본업이 main_td 의 폴러로 옮겨가므로 idle.
 *
 * mutex trylock 으로 race 회피: 채널 생성/파괴와 동시 진행 시 충돌 방지.
 */
static int
bdev_iscsi_no_main_ch_poll(void *arg)
{
	struct bdev_iscsi_lun *lun = arg;
	enum spdk_thread_poller_rc rc = SPDK_POLLER_IDLE;

	if (pthread_mutex_trylock(&lun->mutex)) {        /* [한국어] non-zero = 락 실패 — 채널 변경 진행 중. */
		/* Don't care about the error code here. */
		return SPDK_POLLER_IDLE;                  /* [한국어] 다음 tick 에 재시도. */
	}

	if (lun->ch_count == 0) {                        /* [한국어] 진짜로 채널이 없을 때만 polling. */
		rc = bdev_iscsi_poll_lun(arg);            /* [한국어] 메인 폴러와 동일 로직 재사용. */
	}

	pthread_mutex_unlock(&lun->mutex);                /* [한국어] 락 해제. */
	return rc;
}

/*
 * [한국어]
 * bdev_iscsi_get_buf_cb - READ 의 buf 콜백. bdev 코어가 iov 버퍼를 준비한 후 호출.
 *
 * @ch: io channel.
 * @bdev_io: 처리할 IO.
 * @success: bdev_io_get_buf 가 버퍼 확보에 성공했는지.
 *
 * spdk_bdev_io_get_buf 는 bdev 코어가 iovec 의 iov_base 가 NULL 인 경우 (사용자가
 * payload buffer 를 SPDK 가 관리하도록 위임) 메모리 풀에서 buf 를 할당해주는
 * 메커니즘. 성공 시 readv 로 진행, 실패 시 즉시 완료.
 */
static void
bdev_iscsi_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		      bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);  /* [한국어] 버퍼 할당 실패 — IO 즉시 실패. */
		return;
	}

	/* [한국어] 이제 iov 가 유효 — 실제 SCSI READ_16 발행. nbytes = num_blocks * blocklen. */
	bdev_iscsi_readv((struct bdev_iscsi_lun *)bdev_io->bdev->ctxt,
			 (struct bdev_iscsi_io *)bdev_io->driver_ctx,
			 bdev_io->u.bdev.iovs,
			 bdev_io->u.bdev.iovcnt,
			 bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
			 bdev_io->u.bdev.offset_blocks);
}

/*
 * [한국어]
 * _bdev_iscsi_submit_request - bdev_io 타입별 dispatcher (main_td 컨텍스트).
 *
 * @_bdev_io: 처리할 bdev_io.
 *
 * 본 함수는 항상 main_td 에서 실행됨이 보장된다 (호출자 bdev_iscsi_submit_request
 * 가 send_msg 로 위임하거나, 같은 스레드면 직접 호출). READ 만 get_buf 경유,
 * 나머지는 즉시 libiscsi task 발행.
 */
static void
_bdev_iscsi_submit_request(void *_bdev_io)
{
	struct spdk_bdev_io *bdev_io = _bdev_io;
	struct bdev_iscsi_io *iscsi_io = (struct bdev_iscsi_io *)bdev_io->driver_ctx;
	struct bdev_iscsi_lun *lun = (struct bdev_iscsi_lun *)bdev_io->bdev->ctxt;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		/* [한국어] READ — 사용자가 iov_base=NULL 로 보낸 경우 SPDK 가 버퍼 할당.
		 * 할당 완료 시 bdev_iscsi_get_buf_cb 호출되어 실제 READ_16 발행. */
		spdk_bdev_io_get_buf(bdev_io, bdev_iscsi_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;

	case SPDK_BDEV_IO_TYPE_WRITE:
		/* [한국어] WRITE — buf 이미 있음. 바로 WRITE_16 발행. */
		bdev_iscsi_writev(lun, iscsi_io,
				  bdev_io->u.bdev.iovs,
				  bdev_io->u.bdev.iovcnt,
				  bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				  bdev_io->u.bdev.offset_blocks);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		/* [한국어] FLUSH — SYNCHRONIZE_CACHE_16. IMMED=NO 로 완료 응답을 기다림. */
		bdev_iscsi_flush(lun, iscsi_io,
				 bdev_io->u.bdev.num_blocks,
				 ISCSI_IMMEDIATE_DATA_NO,
				 bdev_io->u.bdev.offset_blocks);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		/* [한국어] RESET — iSCSI TMF LUN_RESET. 별도 함수로 위임. */
		bdev_iscsi_reset(bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		/* [한국어] UNMAP — SCSI UNMAP (단일 descriptor). */
		bdev_iscsi_unmap(lun, iscsi_io,
				 bdev_io->u.bdev.offset_blocks,
				 bdev_io->u.bdev.num_blocks);
		break;
	default:
		/* [한국어] WRITE_ZEROES, COMPARE, NVME_ADMIN 등 미지원. */
		bdev_iscsi_io_complete(iscsi_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

/*
 * [한국어]
 * bdev_iscsi_submit_request - bdev fn_table.submit_request 엔트리.
 *
 * @_ch: 제출하는 io channel — spdk_io_channel_get_thread 로 submit_td 추출.
 * @bdev_io: 처리할 IO.
 *
 * libiscsi 는 단일 스레드 점유가 필요하므로, 제출 스레드가 main_td 가 아니면
 * spdk_thread_send_msg 로 main_td 에 위임. 같은 스레드면 즉시 처리.
 * iscsi_io->lun 과 submit_td 를 채워서 완료 콜백이 올바른 컨텍스트로 디스패치
 * 할 수 있게 함.
 */
static void
bdev_iscsi_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_thread *submit_td = spdk_io_channel_get_thread(_ch);      /* [한국어] 제출 스레드 추출. */
	struct bdev_iscsi_io *iscsi_io = (struct bdev_iscsi_io *)bdev_io->driver_ctx;
	struct bdev_iscsi_lun *lun = (struct bdev_iscsi_lun *)bdev_io->bdev->ctxt;

	iscsi_io->lun = lun;                                                  /* [한국어] 완료 콜백에서 lun 추출 위해 저장. */

	if (lun->main_td != submit_td) {                                      /* [한국어] cross-thread 제출. */
		iscsi_io->submit_td = submit_td;                              /* [한국어] 완료 시 돌아갈 스레드 기록. */
		spdk_thread_send_msg(lun->main_td, _bdev_iscsi_submit_request, bdev_io); /* [한국어] main_td 로 dispatch. */
		return;
	} else {                                                              /* [한국어] 같은 스레드 — fast path. */
		iscsi_io->submit_td = NULL;                                   /* [한국어] 동일 스레드면 complete 도 직접 호출. */
	}

	_bdev_iscsi_submit_request(bdev_io);                                  /* [한국어] 즉시 dispatch. */
}

/*
 * [한국어]
 * bdev_iscsi_io_type_supported - bdev fn_table.io_type_supported. 지원 IO 종류 보고.
 *
 * @ctx: LUN.
 * @io_type: 질의할 IO 종류.
 * @return: true=지원, false=미지원.
 *
 * READ/WRITE/FLUSH/RESET 은 항상 지원, UNMAP 은 target capability 에 따라 동적.
 */
static bool
bdev_iscsi_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct bdev_iscsi_lun *lun = ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:                  /* [한국어] 항상 지원. */
	case SPDK_BDEV_IO_TYPE_WRITE:                 /* [한국어] 항상 지원. */
	case SPDK_BDEV_IO_TYPE_FLUSH:                 /* [한국어] SYNCHRONIZE_CACHE 항상 지원. */
	case SPDK_BDEV_IO_TYPE_RESET:                 /* [한국어] TMF LUN_RESET 항상 지원. */
		return true;

	case SPDK_BDEV_IO_TYPE_UNMAP:                 /* [한국어] target 의 LBP 지원 여부. */
		return lun->unmap_supported;
	default:
		return false;                          /* [한국어] WRITE_ZEROES 등은 미지원. */
	}
}

/*
 * [한국어]
 * bdev_iscsi_create_cb - io_device 의 channel create 콜백. 첫 채널이면 main_td/폴러 셋업.
 *
 * @io_device: spdk_io_device_register 시 등록된 LUN 포인터.
 * @ctx_buf: SPDK 가 미리 할당한 spdk_io_channel 의 ctx 영역 (= bdev_iscsi_io_channel*).
 * @return: 0=성공.
 *
 * 첫 채널이 생성될 때 LUN 의 main_td 가 결정되고 메인 폴러 + timeout 폴러가 등록된다.
 * 두 번째 이후 채널은 ch_count 만 증가시킨다 (모든 채널이 같은 main_td 의 poller 결과를 공유).
 */
static int
bdev_iscsi_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_iscsi_io_channel *ch = ctx_buf;
	struct bdev_iscsi_lun *lun = io_device;

	pthread_mutex_lock(&lun->mutex);                                 /* [한국어] ch_count / main_td 보호. */
	if (lun->ch_count == 0) {                                        /* [한국어] 첫 채널 — 본격 셋업. */
		assert(lun->main_td == NULL);                            /* [한국어] 일관성: 채널 없는데 main_td 가 있으면 버그. */
		lun->main_td = spdk_get_thread();                        /* [한국어] 현재 스레드 = main_td. */
		lun->poller = SPDK_POLLER_REGISTER(bdev_iscsi_poll_lun, lun, 0); /* [한국어] busy-poll (주기=0 → 매 reactor 루프). */
		if (g_opts.timeout_sec > 0) {                            /* [한국어] timeout 사용 모드면. */
			lun->timeout_poller = SPDK_POLLER_REGISTER(bdev_iscsi_poll_lun_timeout, lun,
					      g_opts.timeout_poller_period_us); /* [한국어] 별도 폴러 — timeout 검사만. */
		}
		ch->lun = lun;                                            /* [한국어] 채널 컨텍스트에 LUN 포인터 저장. */
	}
	lun->ch_count++;                                                  /* [한국어] 채널 카운트 증가. */
	pthread_mutex_unlock(&lun->mutex);

	return 0;
}

/*
 * [한국어]
 * _iscsi_destroy_cb - destroy 의 cross-thread 위임 핸들러 (main_td 컨텍스트).
 *
 * @ctx: LUN.
 *
 * destroy_cb 가 main_td 가 아닌 스레드에서 호출되었을 때 main_td 로 send_msg 되어
 * 실행되는 본체. main_td 가 직접 호출했다면 destroy_cb 본체가 처리하므로 이 함수는
 * 호출되지 않음. main_td 에서 폴러 unregister 가 안전.
 */
static void
_iscsi_destroy_cb(void *ctx)
{
	struct bdev_iscsi_lun *lun = ctx;

	pthread_mutex_lock(&lun->mutex);

	assert(lun->main_td == spdk_get_thread());                       /* [한국어] 반드시 main_td 에서 실행. */
	assert(lun->ch_count > 0);                                       /* [한국어] cross-thread 위임 시 ch_count++ 해뒀음. */

	lun->ch_count--;                                                  /* [한국어] 위임 시 임시로 ++ 했던 것 되돌리기. */
	if (lun->ch_count > 0) {                                          /* [한국어] 그 사이 새 채널이 생겼다면 cleanup 보류. */
		pthread_mutex_unlock(&lun->mutex);
		return;
	}

	lun->main_td = NULL;                                              /* [한국어] 더는 main_td 없음. */
	spdk_poller_unregister(&lun->poller);                             /* [한국어] 메인 I/O 폴러 제거. */
	spdk_poller_unregister(&lun->timeout_poller);                     /* [한국어] timeout 폴러 제거 (NULL 이어도 안전). */

	pthread_mutex_unlock(&lun->mutex);
}

/*
 * [한국어]
 * bdev_iscsi_destroy_cb - io_device 의 channel destroy 콜백. 마지막 채널이면 폴러 제거.
 *
 * @io_device: LUN.
 * @ctx_buf: 채널 ctx (사용 안 함).
 *
 * 마지막 채널이 다른 스레드에서 destroy 되면 main_td 로 send_msg → _iscsi_destroy_cb
 * 에서 폴러 제거. 같은 스레드면 직접 unregister.
 */
static void
bdev_iscsi_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_iscsi_lun *lun = io_device;
	struct spdk_thread *thread;

	pthread_mutex_lock(&lun->mutex);
	lun->ch_count--;                                                  /* [한국어] 채널 카운트 감소. */
	if (lun->ch_count == 0) {                                         /* [한국어] 마지막 채널이었음. */
		assert(lun->main_td != NULL);                             /* [한국어] 채널이 있었으니 main_td 도 있음. */

		if (lun->main_td != spdk_get_thread()) {
			/* The final channel was destroyed on a different thread
			 * than where the first channel was created. Pass a message
			 * to the main thread to unregister the poller. */
			/* [한국어] 위 영문 주석: cross-thread 시 main_td 로 위임 필요.
			 * 위임 도중 새 채널 생성 가능성을 막기 위해 ch_count 임시 ++. */
			lun->ch_count++;                                  /* [한국어] _iscsi_destroy_cb 가 다시 -- 함. */
			thread = lun->main_td;
			pthread_mutex_unlock(&lun->mutex);
			spdk_thread_send_msg(thread, _iscsi_destroy_cb, lun); /* [한국어] main_td 로 dispatch. */
			return;
		}

		lun->main_td = NULL;                                       /* [한국어] same-thread 케이스 — 직접 처리. */
		spdk_poller_unregister(&lun->poller);                      /* [한국어] 메인 폴러 제거. */
		spdk_poller_unregister(&lun->timeout_poller);              /* [한국어] timeout 폴러 제거. */
	}
	pthread_mutex_unlock(&lun->mutex);
}

/*
 * [한국어]
 * bdev_iscsi_get_io_channel - bdev fn_table.get_io_channel. 외부에서 채널 획득용.
 *
 * @ctx: LUN.
 * @return: 채널 — spdk_get_io_channel 이 ref-count++ 후 반환.
 */
static struct spdk_io_channel *
bdev_iscsi_get_io_channel(void *ctx)
{
	struct bdev_iscsi_lun *lun = ctx;

	return spdk_get_io_channel(lun);                /* [한국어] io_device(=lun)에 대해 현재 스레드의 채널 획득/생성. */
}

/*
 * [한국어]
 * bdev_iscsi_dump_info_json - bdev_get_bdevs RPC 에서 LUN 정보를 JSON 으로 출력.
 *
 * @ctx: LUN.
 * @w: JSON writer.
 * @return: 0.
 *
 * RPC 결과의 bdev 항목 안에 "driver_specific" : { "iscsi": {...} } 형태로 추가됨.
 */
static int
bdev_iscsi_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_iscsi_lun *lun = ctx;

	spdk_json_write_named_object_begin(w, "iscsi");                            /* [한국어] "iscsi": { ... 시작. */
	spdk_json_write_named_string(w, "initiator_name", lun->initiator_iqn);     /* [한국어] IQN 노출. */
	spdk_json_write_named_string(w, "url", lun->url);                          /* [한국어] iSCSI URL. */
	spdk_json_write_object_end(w);                                             /* [한국어] } 종료. */

	return 0;
}

/*
 * [한국어]
 * bdev_iscsi_write_config_json - 이 LUN 을 재현하기 위한 bdev_iscsi_create RPC 명령 출력.
 *
 * @bdev: 출력 대상.
 * @w: JSON writer.
 *
 * save_config 경로 — 사용자가 SPDK 상태를 저장한 뒤 다시 load 할 때 동일한 bdev 가
 * 재생성되도록 RPC 명령을 JSON 으로 직렬화.
 */
static void
bdev_iscsi_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct bdev_iscsi_lun *lun = bdev->ctxt;

	pthread_mutex_lock(&lun->mutex);                              /* [한국어] url/iqn 읽는 동안 mutate 방지. */
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_iscsi_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_string(w, "initiator_iqn", lun->initiator_iqn);
	spdk_json_write_named_string(w, "url", lun->url);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
	pthread_mutex_unlock(&lun->mutex);
}

/* [한국어] LUN 의 bdev 콜백 vtable. spdk_bdev 의 fn_table 멤버를 통해 bdev 코어가 호출.
 * 모든 외부 인터페이스는 여기 통해 진입. */
static const struct spdk_bdev_fn_table iscsi_fn_table = {
	.destruct		= bdev_iscsi_destruct,            /* [한국어] bdev unregister 시 호출 — async. */
	.submit_request		= bdev_iscsi_submit_request,      /* [한국어] I/O 요청 진입점. */
	.io_type_supported	= bdev_iscsi_io_type_supported,   /* [한국어] capability query. */
	.get_io_channel		= bdev_iscsi_get_io_channel,      /* [한국어] 채널 획득. */
	.dump_info_json		= bdev_iscsi_dump_info_json,      /* [한국어] bdev_get_bdevs 결과의 driver_specific. */
	.write_config_json	= bdev_iscsi_write_config_json,   /* [한국어] config dump — RPC 재현. */
};

/*
 * [한국어]
 * create_iscsi_lun - INQUIRY/READ_CAPACITY 완료 후 최종적으로 spdk_bdev 등록.
 *
 * @req: 연결 요청 — 여기서 ownership (url, name, iqn, context) 이 lun 으로 이전.
 * @num_blocks: target 이 보고한 총 블록 수 (READ_CAPACITY 결과).
 * @block_size: 논리 블록 크기 (보통 512 또는 4096).
 * @bdev: 출력 — 생성된 bdev 포인터.
 * @lbppbe: Logical Blocks Per Physical Block Exponent — physical block 크기 표현.
 *          phys_blocklen = block_size * 2^lbppbe.
 * @return: 0=성공, 음수=실패.
 *
 * io_device 등록 → bdev 등록 → background polling 폴러 등록의 순서. bdev 등록이
 * 실패하면 io_device 도 unregister.
 *
 * 호출 체인: iscsi_readcapacity16_cb → [이 함수].
 */
static int
create_iscsi_lun(struct bdev_iscsi_conn_req *req, uint64_t num_blocks,
		 uint32_t block_size, struct spdk_bdev **bdev, uint8_t lbppbe)
{
	struct bdev_iscsi_lun *lun;
	int rc;

	lun = calloc(1, sizeof(*lun));                            /* [한국어] zero-init LUN 할당. */
	if (!lun) {
		SPDK_ERRLOG("Unable to allocate enough memory for iscsi backend\n");
		return -ENOMEM;
	}

	/* [한국어] ownership 이전: req 에서 lun 으로 포인터 이동. 이후 req 가 해제될 때
	 * 이 필드들은 free 되지 않는다 (conn_poll 에서 status==0 일 때 free(req) 만 함). */
	lun->context = req->context;
	lun->lun_id = req->lun;
	lun->url = req->url;
	lun->initiator_iqn = req->initiator_iqn;

	pthread_mutex_init(&lun->mutex, NULL);                    /* [한국어] 채널 라이프사이클 보호용. */

	lun->bdev.name = req->bdev_name;                          /* [한국어] bdev 이름 — 마찬가지로 ownership 이전. */
	lun->bdev.product_name = "iSCSI LUN";                     /* [한국어] static 문자열 — 사용자에 표시. */
	lun->bdev.module = &g_iscsi_bdev_module;                  /* [한국어] 어느 모듈 소속인지. */
	lun->bdev.blocklen = block_size;                          /* [한국어] 논리 블록 크기. */
	lun->bdev.phys_blocklen = block_size * (1 << lbppbe);     /* [한국어] 물리 블록 크기 — 정렬 힌트. */
	lun->bdev.blockcnt = num_blocks;                          /* [한국어] 총 블록 수. */
	lun->bdev.ctxt = lun;                                     /* [한국어] fn_table 콜백들이 ctx 로 받게 됨. */
	lun->unmap_supported = req->unmap_supported;              /* [한국어] LBP 지원 여부 (INQUIRY 결과). */
	if (lun->unmap_supported) {
		lun->max_unmap = req->max_unmap;                  /* [한국어] target 의 max_unmap LBA 수. */
		lun->bdev.max_unmap = req->max_unmap;             /* [한국어] bdev 코어에도 알려서 상위가 분할. */
		lun->bdev.max_unmap_segments = BDEV_ISCSI_MAX_UNMAP_BLOCK_DESCS_COUNT; /* [한국어] descriptor 1개만. */
	}

	lun->bdev.fn_table = &iscsi_fn_table;                     /* [한국어] vtable 연결 — 이제 bdev 코어가 호출 가능. */

	/* [한국어] io_device 등록 — channel create/destroy 콜백 + per-channel ctx 크기 지정. */
	spdk_io_device_register(lun, bdev_iscsi_create_cb, bdev_iscsi_destroy_cb,
				sizeof(struct bdev_iscsi_io_channel),
				req->bdev_name);
	rc = spdk_bdev_register(&lun->bdev);                      /* [한국어] bdev 코어에 등록 — name 충돌 등 검사. */
	if (rc) {
		spdk_io_device_unregister(lun, NULL);             /* [한국어] 등록 실패 — io_device 도 해제. */
		pthread_mutex_destroy(&lun->mutex);
		free(lun);
		return rc;
	}

	lun->no_main_ch_poller_td = spdk_get_thread();            /* [한국어] background 폴러를 현재 스레드에 바인딩. */
	lun->no_main_ch_poller = SPDK_POLLER_REGISTER(bdev_iscsi_no_main_ch_poll, lun,
				 BDEV_ISCSI_NO_MAIN_CH_POLL_US); /* [한국어] 10ms 주기 — 채널 없을 때만 polling. */

	*bdev = &lun->bdev;                                        /* [한국어] 결과 반환. */
	return 0;
}

/*
 * [한국어]
 * iscsi_readcapacity16_cb - 초기 READ_CAPACITY_16 응답. 최종 lun 생성 단계.
 *
 * @iscsi: 컨텍스트.
 * @status: SCSI status.
 * @command_data: scsi_task*.
 * @private_data: conn_req*.
 *
 * Connect → INQUIRY(LBP) → INQUIRY(Block Limits) → READ_CAPACITY_16 까지 진행되어
 * 마침내 lun 생성 가능한 상태. status 가 CAPACITY_DATA_HAS_CHANGED 면 그 변화에
 * 응답이 의도된 것이므로 한 번 더 READ_CAPACITY 발행.
 *
 * 호출 체인 (전체):
 *   create_iscsi_disk → iscsi_connect_cb → bdev_iscsi_inquiry_lbp_cb
 *     → bdev_iscsi_inquiry_bl_cb → [이 함수] → create_iscsi_lun → complete_conn_req.
 */
static void
iscsi_readcapacity16_cb(struct iscsi_context *iscsi, int status,
			void *command_data, void *private_data)
{
	struct bdev_iscsi_conn_req *req = private_data;
	struct scsi_readcapacity16 *readcap16;
	struct spdk_bdev *bdev = NULL;
	struct scsi_task *task = command_data;
	struct scsi_task *retry_task = NULL;

	if (status != SPDK_SCSI_STATUS_GOOD) {                          /* [한국어] 에러 응답. */
		SPDK_ERRLOG("iSCSI error: %s\n", iscsi_get_error(iscsi));
		if (_bdev_iscsi_is_size_change(status, task)) {         /* [한국어] CAPACITY_DATA_HAS_CHANGED — 재시도 가능. */
			scsi_free_scsi_task(task);                       /* [한국어] 기존 task 해제 후 재발행. */
			retry_task = iscsi_readcapacity16_task(iscsi, req->lun,
							       iscsi_readcapacity16_cb, req);
			if (retry_task) {
				return;                                  /* [한국어] 재발행 성공 — 두 번째 응답 대기. */
			}
		}
		goto ret;                                                /* [한국어] 재발행도 실패 또는 다른 종류 에러. */
	}

	readcap16 = scsi_datain_unmarshall(task);                        /* [한국어] data-in 파싱. */
	if (!readcap16) {
		status = -ENOMEM;                                        /* [한국어] 파싱 실패 → ENOMEM 으로 보고. */
		goto ret;
	}

	/* [한국어] returned_lba + 1 = 총 블록 수, block_length = 블록 크기, lbppbe = physical 정렬 정보. */
	status = create_iscsi_lun(req, readcap16->returned_lba + 1, readcap16->block_length, &bdev,
				  readcap16->lbppbe);
	if (status) {
		SPDK_ERRLOG("Unable to create iscsi bdev: %s (%d)\n", spdk_strerror(-status), status);
	}

ret:
	scsi_free_scsi_task(task);                                       /* [한국어] task 해제 — 모든 경로 공통. */
	complete_conn_req(req, bdev, status);                            /* [한국어] RPC 응답 콜백 호출 + 큐에서 제거. */
}

/*
 * [한국어]
 * bdev_iscsi_inquiry_bl_cb - INQUIRY (Block Limits VPD page) 응답.
 *
 * Block Limits VPD page 의 max_unmap 을 추출하여 req 에 저장 — 이후 lun 생성 시
 * 사용. 응답이 없거나 max_unmap=0 이면 기본값(BDEV_ISCSI_DEFAULT_MAX_UNMAP_LBA_COUNT)
 * 사용. 그 다음 단계로 READ_CAPACITY_16 발행.
 */
static void
bdev_iscsi_inquiry_bl_cb(struct iscsi_context *context, int status, void *_task, void *private_data)
{
	struct scsi_task *task = _task;
	struct scsi_inquiry_block_limits *bl_inq = NULL;
	struct bdev_iscsi_conn_req *req = private_data;

	if (status == SPDK_SCSI_STATUS_GOOD) {
		bl_inq = scsi_datain_unmarshall(task);                  /* [한국어] Block Limits VPD 파싱. */
		if (bl_inq != NULL) {
			if (!bl_inq->max_unmap) {                        /* [한국어] target 이 0 보고 — invalid. */
				SPDK_ERRLOG("Invalid max_unmap, use the default\n");
				req->max_unmap = BDEV_ISCSI_DEFAULT_MAX_UNMAP_LBA_COUNT; /* [한국어] 32768 fallback. */
			} else {
				req->max_unmap = bl_inq->max_unmap;     /* [한국어] target 보고값 채택. */
			}
		}
	}

	scsi_free_scsi_task(task);
	/* [한국어] 다음 단계: READ_CAPACITY_16 — 용량 정보 획득. */
	task = iscsi_readcapacity16_task(context, req->lun, iscsi_readcapacity16_cb, req);
	if (task) {
		return;                                                  /* [한국어] 발행 성공 — 응답 대기. */
	}

	SPDK_ERRLOG("iSCSI error: %s\n", iscsi_get_error(req->context));
	complete_conn_req(req, NULL, status);                           /* [한국어] 실패 — RPC 응답으로 실패 통지. */
}

/*
 * [한국어]
 * bdev_iscsi_inquiry_lbp_cb - INQUIRY (Logical Block Provisioning VPD) 응답.
 *
 * lbpu=1 이면 target 이 UNMAP 지원 — req->unmap_supported=true 설정 후 Block
 * Limits VPD 페이지로 진행하여 max_unmap 조회. UNMAP 미지원이면 바로
 * READ_CAPACITY_16 으로 진행.
 */
static void
bdev_iscsi_inquiry_lbp_cb(struct iscsi_context *context, int status, void *_task,
			  void *private_data)
{
	struct scsi_task *task = _task;
	struct scsi_inquiry_logical_block_provisioning *lbp_inq = NULL;
	struct bdev_iscsi_conn_req *req = private_data;

	if (status == SPDK_SCSI_STATUS_GOOD) {
		lbp_inq = scsi_datain_unmarshall(task);                 /* [한국어] LBP VPD 파싱. */
		if (lbp_inq != NULL && lbp_inq->lbpu) {                  /* [한국어] lbpu=1: UNMAP 지원. */
			req->unmap_supported = true;
			scsi_free_scsi_task(task);

			/* [한국어] Block Limits VPD (페이지 코드 0xB0) 발행 — max_unmap 조회. */
			task = iscsi_inquiry_task(context, req->lun, 1,
						  SCSI_INQUIRY_PAGECODE_BLOCK_LIMITS,
						  255, bdev_iscsi_inquiry_bl_cb, req);
			if (task) {
				return;
			}
		}
	} else {
		scsi_free_scsi_task(task);                               /* [한국어] LBP 페이지 응답이 에러 — UNMAP 비활성. */
	}

	/* [한국어] UNMAP 미지원 / 에러 → 곧바로 용량 조회로 진행. */
	task = iscsi_readcapacity16_task(context, req->lun, iscsi_readcapacity16_cb, req);
	if (task) {
		return;
	}

	SPDK_ERRLOG("iSCSI error: %s\n", iscsi_get_error(req->context));
	complete_conn_req(req, NULL, status);
}

/*
 * [한국어]
 * iscsi_connect_cb - iSCSI full_connect_async 의 login 완료 콜백.
 *
 * login phase (인증 + 협상) 가 끝나면 호출됨. 성공 시 INQUIRY (LBP VPD) 로 진행하여
 * target capability 탐색 시작.
 */
static void
iscsi_connect_cb(struct iscsi_context *iscsi, int status,
		 void *command_data, void *private_data)
{
	struct bdev_iscsi_conn_req *req = private_data;
	struct scsi_task *task;

	if (status != SPDK_SCSI_STATUS_GOOD) {                           /* [한국어] login 실패. */
		goto ret;
	}

	/* [한국어] INQUIRY VPD page 0xB2 (Logical Block Provisioning) 발행. evpd=1, alloc_len=255. */
	task = iscsi_inquiry_task(iscsi, req->lun, 1,
				  SCSI_INQUIRY_PAGECODE_LOGICAL_BLOCK_PROVISIONING,
				  255, bdev_iscsi_inquiry_lbp_cb, req);
	if (task) {
		return;                                                  /* [한국어] 성공 — 다음 콜백 대기. */
	}

ret:
	SPDK_ERRLOG("iSCSI error: %s\n", iscsi_get_error(req->context));
	complete_conn_req(req, NULL, status);                            /* [한국어] 실패 — RPC 응답으로 통지. */
}

/*
 * [한국어]
 * iscsi_bdev_conn_poll - g_iscsi_conn_req 큐의 모든 pending 연결을 polling.
 *
 * @arg: 미사용.
 * @return: BUSY=처리, IDLE=큐 비어 poller 자체 해제.
 *
 * 진행 중인 모든 connect 요청의 libiscsi fd 를 차례대로 poll() → iscsi_service() →
 * 콜백 디스패치. 진행이 끝나거나(req->status==0) 실패(req->status>0)면 req 해제.
 * (실패의 경우 complete_conn_req 가 큐에서 이미 제거했으므로 이 폴러는 free 만 함).
 *
 * 실행 컨텍스트: g_conn_poller 가 등록된 스레드 (보통 main reactor).
 */
static int
iscsi_bdev_conn_poll(void *arg)
{
	struct bdev_iscsi_conn_req *req, *tmp;                          /* [한국어] FOREACH_SAFE cursor. */
	struct pollfd pfd;
	struct iscsi_context *context;

	if (TAILQ_EMPTY(&g_iscsi_conn_req)) {                            /* [한국어] 진행 중인 연결 없음. */
		spdk_poller_unregister(&g_conn_poller);                  /* [한국어] 자기 자신 해제 — lazy lifecycle. */
		return SPDK_POLLER_IDLE;
	}

	TAILQ_FOREACH_SAFE(req, &g_iscsi_conn_req, link, tmp) {          /* [한국어] safe 버전 — 루프 중 제거/해제 허용. */
		context = req->context;
		pfd.fd = iscsi_get_fd(context);                          /* [한국어] 각 req 의 TCP fd. */
		pfd.events = iscsi_which_events(context);                /* [한국어] libiscsi 가 기다리는 이벤트. */
		pfd.revents = 0;                                          /* [한국어] poll 결과 초기화. */
		if (poll(&pfd, 1, 0) < 0) {                              /* [한국어] non-blocking poll. */
			SPDK_ERRLOG("poll failed\n");
			return SPDK_POLLER_BUSY;
		}

		if (pfd.revents != 0) {
			if (iscsi_service(context, pfd.revents) < 0) {    /* [한국어] 송수신 + 콜백 처리. */
				SPDK_ERRLOG("iscsi_service failed: %s\n", iscsi_get_error(context));
			}
		}

		if (req->status == 0) {
			/*
			 * The request completed successfully.
			 */
			/* [한국어] 위 영문: 성공 완료. complete_conn_req 가 이미 url/iqn/name/context
			 * ownership 을 lun 으로 이전했으므로, req 본체만 free. */
			free(req);
		} else if (req->status > 0) {
			/*
			 * An error has occurred during connecting.  This req has already
			 * been removed from the g_iscsi_conn_req list, but we needed to
			 * wait until iscsi_service unwound before we could free the req.
			 */
			/* [한국어] 위 영문 주석대로 — 에러 경로에서는 iscsi_destroy_context 까지 안전하게 호출. */
			_bdev_iscsi_conn_req_free(req);
		}
		/* [한국어] req->status == -1 (진행 중) 이면 계속 polling. */
	}
	return SPDK_POLLER_BUSY;                                         /* [한국어] 매 tick 실제 일을 했음. */
}

/*
 * [한국어]
 * create_iscsi_disk - bdev_iscsi_create RPC 의 핵심 진입점. 비동기 connect 시작.
 *
 * @bdev_name: 생성할 bdev 이름.
 * @url: iSCSI URL — "iscsi://[user[:passwd]@]host[:port]/target-iqn/lun".
 * @initiator_iqn: 본 initiator 의 IQN.
 * @cb_fn: 완료 콜백 — RPC 응답 작성. (NULL 이면 -EINVAL).
 * @cb_arg: cb_fn 의 user_data.
 * @return: 0=시작됨 (완료는 비동기), 음수=시작 실패 (cb_fn 호출 안 됨).
 *
 * 동작 단계:
 *   1. conn_req 할당, strdup 으로 문자열 복사.
 *   2. libiscsi context 생성, URL 파싱.
 *   3. session 옵션 설정 (NORMAL session, no header digest, target name, timeout).
 *   4. iscsi_full_connect_async() — login phase 시작 (TCP connect + 인증).
 *   5. CHAP 인증 정보가 URL 에 있으면 username/password 설정.
 *   6. g_iscsi_conn_req 큐에 등록, g_conn_poller lazy 생성.
 *
 * 호출 체인: rpc_bdev_iscsi_create → [이 함수] → (비동기) iscsi_connect_cb → ...
 */
int
create_iscsi_disk(const char *bdev_name, const char *url, const char *initiator_iqn,
		  spdk_bdev_iscsi_create_cb cb_fn, void *cb_arg)
{
	struct bdev_iscsi_conn_req *req;
	struct iscsi_url *iscsi_url = NULL;
	int rc;

	if (!bdev_name || !url || !initiator_iqn || strlen(initiator_iqn) == 0 || !cb_fn) {
		return -EINVAL;                                          /* [한국어] 필수 인자 검증 — 콜백도 필수. */
	}

	req = calloc(1, sizeof(struct bdev_iscsi_conn_req));             /* [한국어] zero-init conn_req 할당. */
	if (!req) {
		SPDK_ERRLOG("Cannot allocate pointer of struct bdev_iscsi_conn_req\n");
		return -ENOMEM;
	}

	req->status = SCSI_STATUS_GOOD;                                  /* [한국어] 초기 상태 = 0 (성공). 큐 등록 직전에 -1 로 변경. */
	req->bdev_name = strdup(bdev_name);                              /* [한국어] caller 문자열 복사 — 비동기 처리 동안 살아있어야. */
	req->url = strdup(url);
	req->initiator_iqn = strdup(initiator_iqn);
	req->context = iscsi_create_context(initiator_iqn);              /* [한국어] libiscsi 세션 컨텍스트 할당. */
	if (!req->bdev_name || !req->url || !req->initiator_iqn || !req->context) {
		SPDK_ERRLOG("Out of memory\n");
		rc = -ENOMEM;
		goto err;
	}

	req->create_cb = cb_fn;                                          /* [한국어] 완료 콜백 저장. */
	req->create_cb_arg = cb_arg;

	iscsi_url = iscsi_parse_full_url(req->context, url);             /* [한국어] URL 파싱 — user/passwd/host/port/target/lun 분리. */
	if (iscsi_url == NULL) {
		SPDK_ERRLOG("could not parse URL: %s\n", iscsi_get_error(req->context));
		rc = -EINVAL;
		goto err;
	}

	req->lun = iscsi_url->lun;                                       /* [한국어] URL 의 LUN 번호 추출 (예: /0). */
	/* [한국어] short-circuit chain — 첫 실패 후로는 rc != 0 이므로 후속 호출 skip.
	 * NORMAL session = 정상 I/O 세션 (Discovery 가 아닌). */
	rc = iscsi_set_session_type(req->context, ISCSI_SESSION_NORMAL);
	rc = rc ? rc : iscsi_set_header_digest(req->context, ISCSI_HEADER_DIGEST_NONE);   /* [한국어] HDgst CRC 검사 비활성 — 성능. */
	rc = rc ? rc : iscsi_set_targetname(req->context, iscsi_url->target);             /* [한국어] target 의 IQN. */
	rc = rc ? rc : iscsi_set_timeout(req->context, g_opts.timeout_sec);               /* [한국어] task timeout (초). */
	/* [한국어] full_connect_async: TCP 연결 + login phase 시작. 완료 시 iscsi_connect_cb. */
	rc = rc ? rc : iscsi_full_connect_async(req->context, iscsi_url->portal, iscsi_url->lun,
						iscsi_connect_cb, req);
	if (rc == 0 && iscsi_url->user[0] != '\0') {                                       /* [한국어] CHAP 인증 정보 있으면. */
		rc = iscsi_set_initiator_username_pwd(req->context, iscsi_url->user, iscsi_url->passwd);
	}

	if (rc < 0) {
		SPDK_ERRLOG("Failed to connect provided URL=%s: %s\n", url, iscsi_get_error(req->context));
		goto err;
	}

	iscsi_destroy_url(iscsi_url);                                    /* [한국어] URL 파싱 결과 해제 — 정보는 context 가 보관. */
	req->status = -1;                                                /* [한국어] -1 = 진행 중 — conn_poll 이 이 값을 보고 polling 지속. */
	TAILQ_INSERT_TAIL(&g_iscsi_conn_req, req, link);                 /* [한국어] 큐에 등록 — conn_poll 이 polling 시작. */
	if (!g_conn_poller) {                                            /* [한국어] 첫 요청이면 폴러 lazy 생성. */
		g_conn_poller = SPDK_POLLER_REGISTER(iscsi_bdev_conn_poll, NULL, BDEV_ISCSI_CONNECTION_POLL_US);
	}

	return 0;                                                         /* [한국어] 비동기 시작 완료. */

err:
	/* iscsi_destroy_url() is not NULL-proof */
	if (iscsi_url) {
		iscsi_destroy_url(iscsi_url);                            /* [한국어] 위 영문 주석대로 — NULL 체크 필요. */
	}

	if (req->context) {
		iscsi_destroy_context(req->context);                     /* [한국어] libiscsi 세션 해제. */
	}

	free(req->initiator_iqn);
	free(req->bdev_name);
	free(req->url);
	free(req);
	return rc;
}

/*
 * [한국어]
 * delete_iscsi_disk - bdev_iscsi_delete RPC 진입점. bdev 이름으로 unregister.
 *
 * @bdev_name: 삭제할 bdev 이름.
 * @cb_fn: 완료 콜백 — bdev_unregister 가 모든 채널/IO 정리 후 호출 (또는 즉시 실패 시).
 * @cb_arg: cb_fn 의 user_data.
 *
 * spdk_bdev_unregister_by_name 이 모듈 검증(이 모듈 소속의 bdev 인지) 후 unregister
 * 절차 시작. 실패 시 (이름 없음, 잘못된 모듈 등) 즉시 cb_fn(-rc) 으로 통지.
 */
void
delete_iscsi_disk(const char *bdev_name, spdk_delete_iscsi_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(bdev_name, &g_iscsi_bdev_module, cb_fn, cb_arg); /* [한국어] 이름 lookup + unregister 시작. */
	if (rc != 0) {
		cb_fn(cb_arg, rc);                                       /* [한국어] 시작 실패 — 직접 콜백 호출. */
	}
}

/*
 * [한국어]
 * bdev_iscsi_initialize - 모듈 init 콜백. 현재 noop (실제 작업은 RPC 시점에 lazy).
 *
 * @return: 0.
 *
 * 모든 초기화는 사용자가 bdev_iscsi_create RPC 를 호출하는 시점에 lazy 하게 진행.
 * 전역 자원은 g_iscsi_conn_req (정적 초기화) 와 g_conn_poller (lazy) 뿐이므로 init
 * 시점에 할 일이 없다.
 */
static int
bdev_iscsi_initialize(void)
{
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(iscsi_init)
/* [한국어] SPDK 디버그 로그 컴포넌트 "iscsi_init" 등록.
 * SPDK_DEBUGLOG(iscsi_init, ...) 매크로가 이 컴포넌트로 분류된 로그를 출력하며,
 * RPC log_set_flag iscsi_init 또는 spdk_app_opts 의 debug_flags 로 활성화 가능. */
