/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2022 Dell Inc, or its subsidiaries. All rights reserved.
 */

/*
 * [한국어 설명] bdev_nvme 모듈 본체 - SPDK NVMe bdev 구현 (bdev_nvme.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK의 bdev_nvme 모듈 (NVMe 컨트롤러를 SPDK bdev로 노출하는 모듈)의
 * 핵심 구현이다. 9000줄 이상의 거대한 파일로, 다음과 같은 책임을 모두 다룬다:
 *   1) NVMe 컨트롤러 attach/probe/connect/detach 라이프사이클 (PCIe + NVMe-oF).
 *   2) namespace 발견과 SPDK bdev 등록 (멀티패스 그룹화 포함).
 *   3) IO 경로: spdk_bdev_io를 받아 lib/nvme의 spdk_nvme_ns_cmd_*로 변환, qpair에 enqueue,
 *      completion poll, IO 통계 수집.
 *   4) 멀티패스/페일오버: 같은 NQN의 여러 path를 하나의 nvme_bdev로 묶고, 정책(active-passive,
 *      active-active+RR, ANA, queue-depth)에 따라 path 선택. ANA log 처리, AEN 핸들링.
 *   5) reset/enable/disable/failover 컨트롤러 op 비동기 시퀀스.
 *   6) NVMe-oF Discovery 자동 attach 흐름 (CDC 폴링).
 *   7) 핫플러그 PCIe 감지.
 *   8) DH-CHAP/PSK 키 관리 (TLS/인증).
 *   9) write_config_json (save_config) 직렬화.
 *  10) IO 채널 라이프사이클 (per-thread io_path/qpair 캐시).
 *
 * 파일 상단의 매크로들(NVME_CTRLR_LOG_FMT, NVME_*_LOG 매크로)은 컨트롤러/qpair/ns의
 * 식별자(SUBNQN, traddr, cntlid, qid, nsid 등)를 자동으로 로그에 포함시키기 위한
 * 헬퍼다. 모든 로그 메시지가 어떤 컨트롤러/경로/namespace에 대한 것인지 추적 가능.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 SPDK의 4-레이어 NVMe 스택 가운데 가장 가운데 층에 위치한다:
 *   [SPDK bdev 코어 (lib/bdev)]
 *         ↑↓ fn_table 콜백
 *   [bdev_nvme 모듈 (이 파일)]      ← 사용자 IO ↔ NVMe 명령 변환
 *         ↑↓ spdk_nvme_*() API
 *   [lib/nvme NVMe 드라이버]        ← SQ/CQ doorbell, PRP/SGL, fabrics transport
 *         ↑↓ MMIO/TCP/RDMA
 *   [디바이스 (NVMe SSD 또는 NVMe-oF target)]
 *
 * 호출 체인 (대표 IO 경로):
 *   사용자 spdk_bdev_read(bdev_io)
 *     → bdev 코어가 fn_table::submit_request 호출
 *     → bdev_nvme_submit_request (이 파일)
 *     → bdev_nvme_get_io_path (멀티패스 정책으로 path 선택)
 *     → bdev_nvme_readv (lib/nvme의 spdk_nvme_ns_cmd_readv 호출)
 *     → SQ doorbell write → 디바이스
 *     → 디바이스가 CQE 작성, MSI/MSI-X 또는 polled CQ 감지
 *     → bdev_nvme_poll 또는 nvme_poll_group_process_completions
 *     → bdev_nvme_readv_done (lib/nvme이 호출하는 완료 콜백)
 *     → spdk_bdev_io_complete_nvme_status (bdev 코어로 완료 보고)
 *     → 사용자 콜백 호출.
 *
 * 호출 체인 (attach):
 *   bdev_nvme_attach_controller RPC → spdk_bdev_nvme_create
 *     → spdk_nvme_connect_async → 비동기 probe poller
 *     → attach_cb → namespace 순회 → nvme_bdev 생성 → spdk_bdev_register.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: lib/nvme (spdk_nvme_*), lib/bdev (spdk_bdev_*), lib/thread (poller, msg),
 *         lib/accel (CRC32C/Compare 가속), lib/keyring (DH-CHAP 키), lib/opal (SED),
 *         lib/util (uuid, string).
 * - 의존받음: bdev_nvme_rpc.c (RPC), nvme_rpc.c (raw cmd RPC), bdev_nvme_cuse_rpc.c (CUSE),
 *             bdev_mdns_client.c (mDNS), vbdev_opal.c (Opal 보호 vbdev),
 *             외부 SPDK bdev 유저 (lib/bdev → fn_table 콜백).
 * - 데이터 흐름: 사용자 IO → bdev_io → io_path 선택 → lib/nvme qpair → 디바이스 → CQE →
 *               poll → completion cb → bdev_io_complete.
 * - 공유 상태: g_nvme_bdev_ctrlrs (전역 그룹 리스트, app 스레드 변경),
 *             각 nvme_ctrlr/nvme_bdev (mutex 보호 일부 필드),
 *             채널별 io_path 캐시 (해당 스레드에서만 접근, lockless).
 *
 * === 주요 함수/구조체 요약 ===
 * - bdev_nvme_submit_request: bdev fn_table::submit_request 진입점.
 * - bdev_nvme_get_io_path: 멀티패스 정책으로 path 선택.
 * - bdev_nvme_create / bdev_nvme_delete: attach/detach 진입점.
 * - bdev_nvme_reset_ctrlr / bdev_nvme_failover_ctrlr: 컨트롤러 op 본체.
 * - bdev_nvme_poll / bdev_nvme_poll_adminq: completion 폴링 poller.
 * - nvme_bdev_create / nvme_bdev_destruct: SPDK bdev 등록/해제.
 * - bdev_nvme_create_qpair / bdev_nvme_disconnect_qpair: IO qpair 라이프사이클.
 * - bdev_nvme_attach_controller_op_cb 및 다양한 비동기 콜백 체인.
 *
 * 본 파일이 너무 커서(9000+ 라인) 본 주석화 작업에서는 4섹션 상단 블록(이 영역) +
 * 핵심 구조체 §4 + 공개 API §2 헤더 + 핵심 인라인만 추가한다. 헤더 파일
 * (bdev_nvme.h)에서 이미 모든 구조체에 §4 주석을 달았으므로 본 파일에서는
 * 함수 단위 주석에 집중한다.
 */

#include "spdk/stdinc.h"

#include "bdev_nvme.h"             /* [한국어] 모듈 내부 자료구조와 API 선언. */

#include "spdk/accel.h"           /* [한국어] CRC32C, compare 등 가속 (Acceleration framework). */
#include "spdk/config.h"          /* [한국어] SPDK_CONFIG_* 빌드 옵션. */
#include "spdk/endian.h"          /* [한국어] big/little endian 변환 헬퍼. */
#include "spdk/bdev.h"            /* [한국어] bdev 코어 API. */
#include "spdk/json.h"            /* [한국어] JSON write context (save_config 등). */
#include "spdk/keyring.h"         /* [한국어] DH-CHAP/PSK 키 매니저. */
#include "spdk/likely.h"          /* [한국어] likely/unlikely 분기 힌트. */
#include "spdk/nvme.h"            /* [한국어] lib/nvme 공개 API. */
#include "spdk/nvme_ocssd.h"      /* [한국어] OpenChannel SSD 확장. */
#include "spdk/nvme_zns.h"        /* [한국어] Zoned Namespace (ZNS) 확장. */
#include "spdk/opal.h"            /* [한국어] TCG Opal SED API. */
#include "spdk/thread.h"          /* [한국어] poller, message, channel API. */
#include "spdk/trace.h"           /* [한국어] SPDK trace framework. */
#include "spdk/string.h"          /* [한국어] spdk_strerror 등. */
#include "spdk/util.h"            /* [한국어] SPDK_COUNTOF 등 매크로. */
#include "spdk/uuid.h"            /* [한국어] UUID 생성/비교. */

#include "spdk/bdev_module.h"     /* [한국어] bdev 모듈 등록 매크로. */
#include "spdk/log.h"

#include "spdk_internal/usdt.h"        /* [한국어] USDT trace probes (DTrace, eBPF). */
#include "spdk_internal/trace_defs.h"  /* [한국어] 모듈 trace 정의. */

#define NVME_CTRLR_LOG_FMT "%s%s%s:%s,cntlid:%u"
#define NVME_CTRLR_LOG_ARGS(nvme_ctrlr) \
  spdk_nvme_trtype_is_fabrics((nvme_ctrlr)->active_path_id->trid.trtype) ? (nvme_ctrlr)->active_path_id->trid.subnqn : "", \
  spdk_nvme_trtype_is_fabrics((nvme_ctrlr)->active_path_id->trid.trtype) ? "," : "", \
  (nvme_ctrlr)->active_path_id->trid.traddr, \
  (nvme_ctrlr)->active_path_id->trid.trsvcid, \
  spdk_nvme_ctrlr_get_id((nvme_ctrlr)->ctrlr)

#define NVME_BDEV_LOG_FMT "%s,nbdev:%p"
#define NVME_BDEV_LOG_ARGS(nbdev) \
  (nbdev)->disk.name, \
  (nbdev)

#define NVME_QPAIR_LOG_FMT "qid:%u,qpair:%p"
#define NVME_QPAIR_LOG_ARGS(nvme_qpair) \
  spdk_nvme_qpair_get_id((nvme_qpair)->qpair), \
  (nvme_qpair)->qpair

#define NVME_NS_LOG_FMT "nsid:%u,ns:%p,nbdev:%p"
#define NVME_NS_LOG_ARGS(ns) \
  (ns)->id, \
  (ns), \
  (ns)->bdev

#define NVME_CTRLR_LOG(type, ctrlr, format, ...) do { \
	if ((ctrlr)) { \
		SPDK_##type##LOG("["NVME_CTRLR_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS(ctrlr), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG("[null ctrlr] " format, ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_CTRLR_LOG2(type, component, ctrlr, format, ...) do { \
	if ((ctrlr)) { \
		SPDK_##type##LOG(component, "["NVME_CTRLR_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS(ctrlr), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG(component, "[null ctrlr] " format, ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_CTRLR_ERRLOG(ctrlr, format, ...) NVME_CTRLR_LOG(ERR, ctrlr, format, ##__VA_ARGS__)
#define NVME_CTRLR_WARNLOG(ctrlr, format, ...) NVME_CTRLR_LOG(WARN, ctrlr, format, ##__VA_ARGS__)
#define NVME_CTRLR_NOTICELOG(ctrlr, format, ...) NVME_CTRLR_LOG(NOTICE, ctrlr, format, ##__VA_ARGS__)
#define NVME_CTRLR_INFOLOG(ctrlr, format, ...) NVME_CTRLR_LOG2(INFO, bdev_nvme, ctrlr, format, ##__VA_ARGS__)

#define NVME_QPAIR_LOG(type, qpair, format, ...) do { \
	if (!(qpair)) { \
		SPDK_##type##LOG("[null qpair] " format, ##__VA_ARGS__); \
	} else if (!(qpair)->ctrlr) { \
		SPDK_##type##LOG("[null ctrlr,"NVME_QPAIR_LOG_FMT"] " format, NVME_QPAIR_LOG_ARGS(qpair), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG("["NVME_CTRLR_LOG_FMT","NVME_QPAIR_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS((qpair)->ctrlr), NVME_QPAIR_LOG_ARGS(qpair), ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_QPAIR_LOG2(type, component, qpair, format, ...) do { \
	if (!(qpair)) { \
		SPDK_##type##LOG(component, "[null qpair] " format, ##__VA_ARGS__); \
	} else if (!(qpair)->ctrlr) { \
		SPDK_##type##LOG(component, "[null ctrlr,"NVME_QPAIR_LOG_FMT"] " format, NVME_QPAIR_LOG_ARGS(qpair), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG(component, "["NVME_CTRLR_LOG_FMT","NVME_QPAIR_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS((qpair)->ctrlr), NVME_QPAIR_LOG_ARGS(qpair), ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_QPAIR_ERRLOG(qpair, format, ...) NVME_QPAIR_LOG(ERR, qpair, format, ##__VA_ARGS__)
#define NVME_QPAIR_WARNLOG(qpair, format, ...) NVME_QPAIR_LOG(WARN, qpair, format, ##__VA_ARGS__)
#define NVME_QPAIR_NOTICELOG(qpair, format, ...) NVME_QPAIR_LOG(NOTICE, qpair, format, ##__VA_ARGS__)
#define NVME_QPAIR_INFOLOG(qpair, format, ...) NVME_QPAIR_LOG2(INFO, bdev_nvme, qpair, format, ##__VA_ARGS__)

#define NVME_NS_LOG(type, ns, format, ...) do { \
	if (!(ns)) { \
		SPDK_##type##LOG("[null ns] " format, ##__VA_ARGS__); \
	} else if (!(ns)->ctrlr) { \
		SPDK_##type##LOG("[null ctrlr,"NVME_NS_LOG_FMT"] " format, NVME_NS_LOG_ARGS(ns), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG("["NVME_CTRLR_LOG_FMT","NVME_NS_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS((ns)->ctrlr), NVME_NS_LOG_ARGS(ns), ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_NS_LOG2(type, component, ns, format, ...) do { \
	if (!(ns)) { \
		SPDK_##type##LOG(component, "[null ns] " format, ##__VA_ARGS__); \
	} else if (!(ns)->ctrlr) { \
		SPDK_##type##LOG(component, "[null ctrlr,"NVME_NS_LOG_FMT"] " format, NVME_NS_LOG_ARGS(ns), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG(component, "["NVME_CTRLR_LOG_FMT","NVME_NS_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS((ns)->ctrlr), NVME_NS_LOG_ARGS(ns), ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_NS_ERRLOG(ns, format, ...) NVME_NS_LOG(ERR, ns, format, ##__VA_ARGS__)
#define NVME_NS_WARNLOG(ns, format, ...) NVME_NS_LOG(WARN, ns, format, ##__VA_ARGS__)
#define NVME_NS_NOTICELOG(ns, format, ...) NVME_NS_LOG(NOTICE, ns, format, ##__VA_ARGS__)
#define NVME_NS_INFOLOG(ns, format, ...) NVME_NS_LOG2(INFO, bdev_nvme, ns, format, ##__VA_ARGS__)

#define NVME_BDEV_LOG(type, nbdev, ctrlr, format, ...) do { \
	if (!(nbdev)) { \
		SPDK_##type##LOG("[null nbdev] " format, ##__VA_ARGS__); \
	} else if (!(ctrlr)) { \
		SPDK_##type##LOG("[null ctrlr,"NVME_BDEV_LOG_FMT"] " format, NVME_BDEV_LOG_ARGS(nbdev), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG("["NVME_CTRLR_LOG_FMT","NVME_BDEV_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS(ctrlr), NVME_BDEV_LOG_ARGS(nbdev), ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_BDEV_LOG2(type, component, nbdev, ctrlr, format, ...) do { \
	if (!(nbdev)) { \
		SPDK_##type##LOG(component, "[null nbdev] " format, ##__VA_ARGS__); \
	} else if (!(ctrlr)) { \
		SPDK_##type##LOG(component, "[null ctrlr,"NVME_BDEV_LOG_FMT"] " format, NVME_BDEV_LOG_ARGS(nbdev), ##__VA_ARGS__); \
	} else { \
		SPDK_##type##LOG(component, "["NVME_CTRLR_LOG_FMT","NVME_BDEV_LOG_FMT"] " format, NVME_CTRLR_LOG_ARGS(ctrlr), NVME_BDEV_LOG_ARGS(nbdev), ##__VA_ARGS__); \
	} \
} while (0)

#define NVME_BDEV_ERRLOG(nbdev, ctrlr, format, ...) NVME_BDEV_LOG(ERR, nbdev, ctrlr, format, ##__VA_ARGS__)
#define NVME_BDEV_WARNLOG(nbdev, ctrlr, format, ...) NVME_BDEV_LOG(WARN, nbdev, ctrlr, format, ##__VA_ARGS__)
#define NVME_BDEV_NOTICELOG(nbdev, ctrlr, format, ...) NVME_BDEV_LOG(NOTICE, nbdev, ctrlr, format, ##__VA_ARGS__)
#define NVME_BDEV_INFOLOG(nbdev, ctrlr, format, ...) NVME_BDEV_LOG2(INFO, bdev_nvme, nbdev, ctrlr, format, ##__VA_ARGS__)

#ifdef DEBUG
#define NVME_CTRLR_DEBUGLOG(ctrlr, format, ...) NVME_CTRLR_LOG2(DEBUG, bdev_nvme, ctrlr, format, ##__VA_ARGS__)
#define NVME_QPAIR_DEBUGLOG(qpair, format, ...) NVME_QPAIR_LOG2(DEBUG, bdev_nvme, qpair, format, ##__VA_ARGS__)
#define NVME_NS_DEBUGLOG(ns, format, ...) NVME_NS_LOG2(DEBUG, bdev_nvme, ns, format, ##__VA_ARGS__)
#define NVME_BDEV_DEBUGLOG(nbdev, ctrlr, format, ...) NVME_BDEV_LOG2(DEBUG, bdev_nvme, nbdev, ctrlr, format, ##__VA_ARGS__)
#else
#define NVME_CTRLR_DEBUGLOG(...) do { } while (0)
#define NVME_QPAIR_DEBUGLOG(...) do { } while (0)
#define NVME_NS_DEBUGLOG(...) do { } while (0)
#define NVME_BDEV_DEBUGLOG(...) do { } while (0)
#endif

#define SPDK_BDEV_NVME_DEFAULT_DELAY_CMD_SUBMIT true
#define SPDK_BDEV_NVME_DEFAULT_KEEP_ALIVE_TIMEOUT_IN_MS	(10000)

#define NSID_STR_LEN 10

#define SPDK_CONTROLLER_NAME_MAX 512

/* A NULL pointer is used for NVME_ log macros when the ctrlr object is not available by design.
 * Since we use macros, we cannot pass NULL directly - we need a pointer of the specific type to
 * suppress compiler warnings. */
static struct nvme_ctrlr *null_ctrlr;

static int bdev_nvme_config_json(struct spdk_json_write_ctx *w);

/*
 * [한국어]
 * struct nvme_bdev_io - bdev_io의 driver_ctx로 임베드되는 bdev_nvme 측 컨텍스트.
 *
 * SPDK bdev 코어가 spdk_bdev_io 끝에 (모듈이 알린 get_ctx_size 만큼) 추가 공간을
 * 할당하고 driver_ctx로 노출. bdev_nvme는 그 공간에 이 구조체를 두어 IO 진행 중에
 * 필요한 모든 상태(iov 위치, retry, fused cmd 상태, NVMe CPL 사본 등)를 보관한다.
 * 한 IO당 1개 인스턴스, IO 발행 시 0-init되어 사용.
 */
struct nvme_bdev_io {
	/** array of iovecs to transfer. */
	struct iovec *iovs;
	/* [한국어] 사용자가 전달한 데이터 iovec 배열 포인터.
	 * 설정자: bdev_nvme_submit_request가 bdev_io에서 추출.
	 * 읽는 자: NVMe 명령 발행 시 SGL 변환 헬퍼. */

	/** Number of iovecs in iovs array. */
	int iovcnt;
	/* [한국어] iovs 원소 개수. */

	/** Current iovec position. */
	int iovpos;
	/* [한국어] 현재 처리 중인 iovec 인덱스 (multi-vector NVMe 발행 시 진행 추적). */

	/** Offset in current iovec. */
	uint32_t iov_offset;
	/* [한국어] iovs[iovpos] 내 바이트 오프셋. */

	/** Offset in current iovec. */
	uint32_t fused_iov_offset;
	/* [한국어] fused command(예: Compare-and-Write)에서 두 번째 op의 iov 오프셋. */

	/** array of iovecs to transfer. */
	struct iovec *fused_iovs;
	/* [한국어] fused command의 두 번째 op용 iovec 배열. */

	/** Number of iovecs in iovs array. */
	int fused_iovcnt;
	/* [한국어] fused_iovs 원소 개수. */

	/** Current iovec position. */
	int fused_iovpos;
	/* [한국어] fused_iovs 진행 인덱스. */

	/** I/O path the current I/O or admin passthrough is submitted on, or the I/O path
	 *  being reset in a reset I/O.
	 */
	struct nvme_io_path *io_path;
	/* [한국어] 이 IO가 발행된 path (멀티패스 중 어느 컨트롤러/qpair인지).
	 * Reset IO의 경우엔 reset 대상 path. completion 처리에서 path별 통계 갱신에 사용. */

	/** Saved status for admin passthru completion event, PI error verification, or intermediate compare-and-write status */
	struct spdk_nvme_cpl cpl;
	/* [한국어] 디바이스가 반환한 16바이트 CPL 사본.
	 * Admin passthrough 응답을 사용자에게 돌려주거나 fused 중간 상태 저장 등에 사용. */

	/** Extended IO opts passed by the user to bdev layer and mapped to NVME format */
	struct spdk_nvme_ns_cmd_ext_io_opts ext_opts;
	/* [한국어] 사용자가 전달한 확장 IO 옵션 (DIF/DIX, namespace metadata 등)을
	 * lib/nvme 형식으로 매핑한 구조체. */

	/** Keeps track if first of fused commands was submitted */
	bool first_fused_submitted;
	/* [한국어] Compare-and-Write의 첫 op(Compare)이 발행되었는가? */

	/** Keeps track if first of fused commands was completed */
	bool first_fused_completed;
	/* [한국어] 첫 op이 완료되었는가? completion 시 두 번째 op로 진행 결정. */

	/* How many times the current I/O was retried. */
	int32_t retry_count;
	/* [한국어] 일시적 실패(qpair busy, ANS 변경 등)로 재시도된 횟수.
	 * bdev_retry_count 옵션 초과 시 IO 영구 실패로 보고. */

	/** Expiration value in ticks to retry the current I/O. */
	uint64_t retry_ticks;
	/* [한국어] 다음 재시도 가능 시각 (TSC). retry_io_poller가 이 값 이상이 되면 재발행. */

	/** Temporary pointer to zone report buffer */
	struct spdk_nvme_zns_zone_report *zone_report_buf;
	/* [한국어] ZNS Zone Report 명령의 응답 버퍼 임시 포인터. */

	/** Keep track of how many zones that have been copied to the spdk_bdev_zone_info struct */
	uint64_t handled_zones;
	/* [한국어] Zone Report 결과를 사용자 형식으로 복사한 zone 개수 (멀티 zone 처리). */

	/* Current tsc at submit time. */
	uint64_t submit_tsc;
	/* [한국어] IO 발행 시점 TSC. timeout 비교, latency 통계에 사용. */

	/* Used to put nvme_bdev_io into the list */
	TAILQ_ENTRY(nvme_bdev_io) retry_link;
	/* [한국어] retry_io_list 또는 pending_resets 큐의 노드. */
};

/*
 * [한국어]
 * struct nvme_probe_skip_entry - 핫플러그 감지 시 무시할 컨트롤러 trid.
 *
 * 사용자가 RPC로 detach한 컨트롤러를 핫플러그 monitor가 다시 자동 attach 하지 않도록
 * skip list에 등록. PCIe NVMe 환경에서 detach 후 디바이스가 그대로 꽂혀 있어도
 * 자동 재attach를 막는다.
 */
struct nvme_probe_skip_entry {
	struct spdk_nvme_transport_id		trid;
	/* [한국어] skip 대상 컨트롤러의 trid. */
	TAILQ_ENTRY(nvme_probe_skip_entry)	tailq;
	/* [한국어] g_skipped_nvme_ctrlrs 리스트 노드. */
};

typedef void (*nvme_ctrlr_put_ref_cb)(struct nvme_ctrlr *nvme_ctrlr);

/* All the controllers deleted by users via RPC are skipped by hotplug monitor */
static TAILQ_HEAD(, nvme_probe_skip_entry) g_skipped_nvme_ctrlrs = TAILQ_HEAD_INITIALIZER(
			g_skipped_nvme_ctrlrs);

#define BDEV_NVME_DEFAULT_DIGESTS (SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA256) | \
				   SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA384) | \
				   SPDK_BIT(SPDK_NVMF_DHCHAP_HASH_SHA512))

#define BDEV_NVME_DEFAULT_DHGROUPS (SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_NULL) | \
				    SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_2048) | \
				    SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_3072) | \
				    SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_4096) | \
				    SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_6144) | \
				    SPDK_BIT(SPDK_NVMF_DHCHAP_DHGROUP_8192))

static struct spdk_bdev_nvme_opts g_opts = {
	.action_on_timeout = SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE,
	.keep_alive_timeout_ms = SPDK_BDEV_NVME_DEFAULT_KEEP_ALIVE_TIMEOUT_IN_MS,
	.timeout_us = 0,
	.timeout_admin_us = 0,
	.transport_retry_count = 4,
	.arbitration_burst = 0,
	.low_priority_weight = 0,
	.medium_priority_weight = 0,
	.high_priority_weight = 0,
	.io_queue_requests = 0,
	.nvme_adminq_poll_period_us = 10000ULL,
	.nvme_ioq_poll_period_us = 0,
	.delay_cmd_submit = SPDK_BDEV_NVME_DEFAULT_DELAY_CMD_SUBMIT,
	.bdev_retry_count = 3,
	.ctrlr_loss_timeout_sec = 0,
	.reconnect_delay_sec = 0,
	.fast_io_fail_timeout_sec = 0,
	.transport_ack_timeout = 0,
	.disable_auto_failback = false,
	.generate_uuids = false,
	.transport_tos = 0,
	.nvme_error_stat = false,
	.io_path_stat = false,
	.allow_accel_sequence = false,
	.dhchap_digests = BDEV_NVME_DEFAULT_DIGESTS,
	.dhchap_dhgroups = BDEV_NVME_DEFAULT_DHGROUPS,
	.rdma_umr_per_io = false,
	.enable_flush = false,
};

#define NVME_HOTPLUG_POLL_PERIOD_MAX			10000000ULL
#define NVME_HOTPLUG_POLL_PERIOD_DEFAULT		100000ULL

static int g_hot_insert_nvme_controller_index = 0;
static uint64_t g_nvme_hotplug_poll_period_us = NVME_HOTPLUG_POLL_PERIOD_DEFAULT;
static bool g_nvme_hotplug_enabled = false;
bool g_bdev_nvme_init_done;
static struct spdk_poller *g_hotplug_poller;
static struct spdk_poller *g_hotplug_probe_poller;
static struct spdk_nvme_probe_ctx *g_hotplug_probe_ctx;

static void nvme_ctrlr_populate_namespaces(struct nvme_ctrlr *nvme_ctrlr,
		struct nvme_async_probe_ctx *ctx);
static void nvme_ctrlr_populate_namespaces_done(struct nvme_ctrlr *nvme_ctrlr,
		struct nvme_async_probe_ctx *ctx);
static int bdev_nvme_init(void);
static void bdev_nvme_fini(void);
static void _bdev_nvme_submit_request(struct nvme_bdev_channel *nbdev_ch,
				      struct spdk_bdev_io *bdev_io);
static void bdev_nvme_submit_request(struct spdk_io_channel *ch,
				     struct spdk_bdev_io *bdev_io);
static int bdev_nvme_readv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
			   void *md, uint64_t lba_count, uint64_t lba,
			   uint32_t flags, struct spdk_memory_domain *domain, void *domain_ctx,
			   struct spdk_accel_sequence *seq);
static int bdev_nvme_no_pi_readv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
				 void *md, uint64_t lba_count, uint64_t lba);
static int bdev_nvme_writev(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
			    void *md, uint64_t lba_count, uint64_t lba,
			    uint32_t flags, struct spdk_memory_domain *domain, void *domain_ctx,
			    struct spdk_accel_sequence *seq,
			    union spdk_bdev_nvme_cdw12 cdw12, union spdk_bdev_nvme_cdw13 cdw13);
static int bdev_nvme_zone_appendv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
				  void *md, uint64_t lba_count,
				  uint64_t zslba, uint32_t flags);
static int bdev_nvme_comparev(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
			      void *md, uint64_t lba_count, uint64_t lba,
			      uint32_t flags);
static int bdev_nvme_comparev_and_writev(struct nvme_bdev_io *bio,
		struct iovec *cmp_iov, int cmp_iovcnt, struct iovec *write_iov,
		int write_iovcnt, void *md, uint64_t lba_count, uint64_t lba,
		uint32_t flags);
static int bdev_nvme_write_uncorrectable(struct nvme_bdev_io *bio, uint64_t lba_count,
		uint64_t lba);
static int bdev_nvme_get_zone_info(struct nvme_bdev_io *bio, uint64_t zone_id,
				   uint32_t num_zones, struct spdk_bdev_zone_info *info);
static int bdev_nvme_zone_management(struct nvme_bdev_io *bio, uint64_t zone_id,
				     enum spdk_bdev_zone_action action);
static void bdev_nvme_admin_passthru(struct nvme_bdev_channel *nbdev_ch,
				     struct nvme_bdev_io *bio,
				     struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes);
static int bdev_nvme_io_passthru(struct nvme_bdev_io *bio, struct spdk_nvme_cmd *cmd,
				 void *buf, size_t nbytes);
static int bdev_nvme_io_passthru_md(struct nvme_bdev_io *bio, struct spdk_nvme_cmd *cmd,
				    void *buf, size_t nbytes, void *md_buf, size_t md_len);
static int bdev_nvme_iov_passthru_md(struct nvme_bdev_io *bio, struct spdk_nvme_cmd *cmd,
				     struct iovec *iov, int iovcnt, size_t nbytes,
				     void *md_buf, size_t md_len);
static void bdev_nvme_abort(struct nvme_bdev_channel *nbdev_ch,
			    struct nvme_bdev_io *bio, struct nvme_bdev_io *bio_to_abort);
static void bdev_nvme_reset_io(struct nvme_bdev *nbdev, struct nvme_bdev_io *bio);
static int bdev_nvme_reset_ctrlr(struct nvme_ctrlr *nvme_ctrlr);
static int bdev_nvme_failover_ctrlr(struct nvme_ctrlr *nvme_ctrlr);
static void remove_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr);
static int nvme_ctrlr_read_ana_log_page(struct nvme_ctrlr *nvme_ctrlr);

static void nvme_ns_free(struct nvme_ns *ns);
static void nvme_ns_delete(struct nvme_ns *ns);

/*
 * [한국어]
 * nvme_ns_cmp - RB-tree 비교 함수 (key: nsid).
 * 트리는 nvme_ctrlr::namespaces. RB_FIND/RB_INSERT가 이 함수로 정렬.
 */
static int
nvme_ns_cmp(struct nvme_ns *ns1, struct nvme_ns *ns2)
{
	return ns1->id < ns2->id ? -1 : ns1->id > ns2->id;
}

/* [한국어] RB-tree 매크로 정의: nvme_ns_tree 타입에 nvme_ns의 node 멤버를 사용해
 * nvme_ns_cmp로 정렬되는 정적(static) 함수들 (RB_FIND/INSERT/REMOVE 등)을 생성. */
RB_GENERATE_STATIC(nvme_ns_tree, nvme_ns, node, nvme_ns_cmp);

/*
 * [한국어]
 * bdev_nvme_get_io_qpair - bdev_nvme.h §2 참조. 채널 컨텍스트에서 lib/nvme qpair 추출.
 *
 * spdk_get_io_channel(nvme_ctrlr) → 채널 핸들 → spdk_io_channel_get_ctx로 우리의
 * nvme_ctrlr_channel 구조체 → 그 안의 qpair → spdk_nvme_qpair * 반환.
 * NVMe raw cmd passthrough 등 외부에서 qpair에 직접 명령 발행할 때 사용.
 */
struct spdk_nvme_qpair *
bdev_nvme_get_io_qpair(struct spdk_io_channel *ctrlr_io_ch)
{
	struct nvme_ctrlr_channel *ctrlr_ch;

	assert(ctrlr_io_ch != NULL);

	ctrlr_ch = spdk_io_channel_get_ctx(ctrlr_io_ch);

	return ctrlr_ch->qpair->qpair;
}

/*
 * [한국어]
 * bdev_nvme_get_ctx_size - SPDK bdev 모듈 콜백: bdev_io::driver_ctx 크기 알림.
 * 반환값만큼의 공간이 자동 할당되어 nvme_bdev_io로 사용된다.
 */
static int
bdev_nvme_get_ctx_size(void)
{
	return sizeof(struct nvme_bdev_io);
}

/*
 * [한국어] bdev_nvme SPDK bdev 모듈 등록 정보.
 *
 * - name: 모듈 식별자.
 * - async_fini: true → fini가 비동기 (모든 컨트롤러 detach 완료까지 대기).
 * - module_init/fini: subsystem init/fini 콜백.
 * - config_json: save_config 시 호출 (모든 attach 명령을 다시 만들어주는 JSON 출력).
 * - get_ctx_size: driver_ctx 크기 보고.
 */
static struct spdk_bdev_module nvme_if = {
	.name = "nvme",
	.async_fini = true,
	.module_init = bdev_nvme_init,
	.module_fini = bdev_nvme_fini,
	.config_json = bdev_nvme_config_json,
	.get_ctx_size = bdev_nvme_get_ctx_size,

};
SPDK_BDEV_MODULE_REGISTER(nvme, &nvme_if)
/* [한국어] ↑ SPDK bdev 시스템에 "nvme" 모듈을 컴파일 타임 등록. 부팅 시 module_init 호출. */

/* [한국어] 모든 nvme_bdev_ctrlr (=멀티패스 그룹)의 전역 헤드.
 * 설정자: bdev_nvme_create/_delete (app 스레드).
 * 읽는 자: 모든 lookup, save_config, RPC 응답.
 * 동기화: app 스레드 외 접근 금지. */
struct nvme_bdev_ctrlrs g_nvme_bdev_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_bdev_ctrlrs);
/* [한국어] 모듈 fini가 진행 중인가? true이면 새 attach/등록 거부. */
bool g_bdev_nvme_module_finish;

/*
 * [한국어]
 * nvme_bdev_ctrlr_get_by_name - bdev_nvme.h §2 참조. 이름으로 멀티패스 그룹 lookup.
 * 컨텍스트: app 스레드 (전역 리스트 접근).
 */
struct nvme_bdev_ctrlr *
nvme_bdev_ctrlr_get_by_name(const char *name)
{
	struct nvme_bdev_ctrlr *nbdev_ctrlr;

	TAILQ_FOREACH(nbdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		if (strcmp(name, nbdev_ctrlr->name) == 0) {
			break;
		}
	}

	return nbdev_ctrlr;
}

static struct nvme_ctrlr *
nvme_bdev_ctrlr_get_ctrlr(struct nvme_bdev_ctrlr *nbdev_ctrlr,
			  const struct spdk_nvme_transport_id *trid, const char *hostnqn)
{
	const struct spdk_nvme_ctrlr_opts *opts;
	struct nvme_ctrlr *nvme_ctrlr;

	TAILQ_FOREACH(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq) {
		opts = spdk_nvme_ctrlr_get_opts(nvme_ctrlr->ctrlr);
		if (spdk_nvme_transport_id_compare(trid, &nvme_ctrlr->active_path_id->trid) == 0 &&
		    strcmp(hostnqn, opts->hostnqn) == 0) {
			break;
		}
	}

	return nvme_ctrlr;
}

/*
 * [한국어]
 * nvme_bdev_ctrlr_get_ctrlr_by_id - bdev_nvme.h §2 참조.
 * cntlid는 NVMe Identify Controller로부터 얻는 값. 멀티패스 그룹 내에서 cntlid는 유일.
 */
struct nvme_ctrlr *
nvme_bdev_ctrlr_get_ctrlr_by_id(struct nvme_bdev_ctrlr *nbdev_ctrlr,
				uint16_t cntlid)
{
	struct nvme_ctrlr *nvme_ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;

	TAILQ_FOREACH(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq) {
		cdata = spdk_nvme_ctrlr_get_data(nvme_ctrlr->ctrlr);
		if (cdata->cntlid == cntlid) {
			break;
		}
	}

	return nvme_ctrlr;
}

static struct nvme_bdev *
nvme_bdev_ctrlr_get_bdev(struct nvme_bdev_ctrlr *nbdev_ctrlr, uint32_t nsid)
{
	struct nvme_bdev *nbdev;

	assert(spdk_thread_is_app_thread(NULL));

	TAILQ_FOREACH(nbdev, &nbdev_ctrlr->bdevs, tailq) {
		if (nbdev->nsid == nsid) {
			break;
		}
	}

	return nbdev;
}

/*
 * [한국어]
 * nvme_ctrlr_get_ns - bdev_nvme.h §2 참조. nsid로 namespace 조회 (RB-tree, O(log n)).
 *
 * 임시 ns 변수에 id만 채워서 RB_FIND의 키로 사용 (red-black 비교 함수가 id 비교).
 * app 스레드 외 호출 금지 (assert).
 */
struct nvme_ns *
nvme_ctrlr_get_ns(struct nvme_ctrlr *nvme_ctrlr, uint32_t nsid)
{
	struct nvme_ns ns;

	assert(nsid > 0);
	assert(spdk_thread_is_app_thread(NULL));

	ns.id = nsid;
	return RB_FIND(nvme_ns_tree, &nvme_ctrlr->namespaces, &ns);
}

static struct nvme_ctrlr *
nvme_ctrlr_get(const struct spdk_nvme_transport_id *trid, const char *hostnqn)
{
	struct nvme_bdev_ctrlr	*nbdev_ctrlr;
	struct nvme_ctrlr	*nvme_ctrlr = NULL;

	assert(spdk_thread_is_app_thread(NULL));

	TAILQ_FOREACH(nbdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		nvme_ctrlr = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, trid, hostnqn);
		if (nvme_ctrlr != NULL) {
			break;
		}
	}

	return nvme_ctrlr;
}

/*
 * [한국어]
 * nvme_ctrlr_get_by_name - bdev_nvme.h §2 참조. 이름으로 컨트롤러 lookup.
 *
 * 멀티패스 그룹의 첫 컨트롤러를 반환 (단일 컨트롤러이면 그 컨트롤러).
 * 컨텍스트: app 스레드.
 */
struct nvme_ctrlr *
nvme_ctrlr_get_by_name(const char *name)
{
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr = NULL;

	assert(spdk_thread_is_app_thread(NULL));

	if (name == NULL) {
		return NULL;
	}

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (nbdev_ctrlr != NULL) {
		nvme_ctrlr = TAILQ_FIRST(&nbdev_ctrlr->ctrlrs);
	}

	return nvme_ctrlr;
}

/*
 * [한국어]
 * nvme_bdev_ctrlr_for_each - bdev_nvme.h §2 참조. 모든 nvme_bdev_ctrlr 순회.
 *
 * 단순 동기 순회 (각 ctrlr에 대해 fn 호출). RPC bdev_nvme_get_controllers 등에서 사용.
 * 컨텍스트: app 스레드 (전역 리스트).
 */
void
nvme_bdev_ctrlr_for_each(nvme_bdev_ctrlr_for_each_fn fn, void *ctx)
{
	struct nvme_bdev_ctrlr *nbdev_ctrlr;

	assert(spdk_thread_is_app_thread(NULL));

	TAILQ_FOREACH(nbdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		fn(nbdev_ctrlr, ctx);
	}
}

/*
 * [한국어]
 * struct nvme_ctrlr_channel_iter - for_each_channel iterator의 사용자 코드 측 컨텍스트.
 * 사용자 콜백(fn)/완료 콜백(cpl)/SPDK iter 핸들/사용자 ctx를 묶음.
 */
struct nvme_ctrlr_channel_iter {
	nvme_ctrlr_for_each_channel_msg fn;
	/* [한국어] 각 채널에서 호출될 사용자 콜백. */
	nvme_ctrlr_for_each_channel_done cpl;
	/* [한국어] 모든 채널 처리 완료 후 1회 호출되는 사용자 콜백. */
	struct spdk_io_channel_iter *i;
	/* [한국어] SPDK 코어가 만든 iterator 핸들. continue/get_*에 인자로 사용. */
	void *ctx;
	/* [한국어] 사용자 컨텍스트 (fn/cpl 모두에 전달됨). */
};

/*
 * [한국어]
 * nvme_ctrlr_for_each_channel_continue - bdev_nvme.h §2 참조.
 * 사용자 fn이 채널 처리 후 다음으로 진행하라고 알리는 헬퍼.
 */
void
nvme_ctrlr_for_each_channel_continue(struct nvme_ctrlr_channel_iter *iter, int status)
{
	spdk_for_each_channel_continue(iter->i, status);
}

static void
nvme_ctrlr_each_channel_msg(struct spdk_io_channel_iter *i)
{
	struct nvme_ctrlr_channel_iter *iter = spdk_io_channel_iter_get_ctx(i);
	struct nvme_ctrlr *nvme_ctrlr = spdk_io_channel_iter_get_io_device(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_ctrlr_channel *ctrlr_ch = spdk_io_channel_get_ctx(ch);

	iter->i = i;
	iter->fn(iter, nvme_ctrlr, ctrlr_ch, iter->ctx);
}

static void
nvme_ctrlr_each_channel_cpl(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_ctrlr_channel_iter *iter = spdk_io_channel_iter_get_ctx(i);
	struct nvme_ctrlr *nvme_ctrlr = spdk_io_channel_iter_get_io_device(i);

	iter->i = i;
	iter->cpl(nvme_ctrlr, iter->ctx, status);

	free(iter);
}

/*
 * [한국어]
 * nvme_ctrlr_for_each_channel - bdev_nvme.h §2 참조. 모든 SPDK 스레드의 ctrlr 채널 순회.
 *
 * SPDK 코어의 spdk_for_each_channel을 감싸고, 우리 측 iter 컨텍스트를 묶어서 전달.
 * 호출자: reset, qpair disconnect, ANA log 갱신 등 모든 채널이 동시에 처리해야 하는 작업.
 * 동작: iter 할당 → spdk_for_each_channel 호출 → 각 채널 스레드에서 our_msg 호출 →
 *        msg 콜백이 fn을 호출, 사용자가 작업 후 _continue 호출 → 다음 채널 → 모두 끝나면 cpl.
 */
void
nvme_ctrlr_for_each_channel(struct nvme_ctrlr *nvme_ctrlr,
			    nvme_ctrlr_for_each_channel_msg fn, void *ctx,
			    nvme_ctrlr_for_each_channel_done cpl)
{
	struct nvme_ctrlr_channel_iter *iter;

	assert(nvme_ctrlr != NULL && fn != NULL);

	iter = calloc(1, sizeof(struct nvme_ctrlr_channel_iter));
	if (iter == NULL) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "Unable to allocate iterator\n");
		assert(false);
		return;
	}

	iter->fn = fn;
	iter->cpl = cpl;
	iter->ctx = ctx;

	spdk_for_each_channel(nvme_ctrlr, nvme_ctrlr_each_channel_msg,
			      iter, nvme_ctrlr_each_channel_cpl);
}

/*
 * [한국어]
 * struct nvme_bdev_channel_iter - bdev 채널 for_each iterator 컨텍스트.
 * nvme_ctrlr_channel_iter와 동일 패턴. nvme_bdev 단위.
 */
struct nvme_bdev_channel_iter {
	nvme_bdev_for_each_channel_msg fn;
	nvme_bdev_for_each_channel_done cpl;
	struct spdk_io_channel_iter *i;
	void *ctx;
};

/* [한국어] bdev 채널 순회 진행 헬퍼 (bdev_nvme.h §2 참조). */
void
nvme_bdev_for_each_channel_continue(struct nvme_bdev_channel_iter *iter, int status)
{
	spdk_for_each_channel_continue(iter->i, status);
}

static void
nvme_bdev_each_channel_msg(struct spdk_io_channel_iter *i)
{
	struct nvme_bdev_channel_iter *iter = spdk_io_channel_iter_get_ctx(i);
	struct nvme_bdev *nbdev = spdk_io_channel_iter_get_io_device(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_bdev_channel *nbdev_ch = spdk_io_channel_get_ctx(ch);

	iter->i = i;
	iter->fn(iter, nbdev, nbdev_ch, iter->ctx);
}

static void
nvme_bdev_each_channel_cpl(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_bdev_channel_iter *iter = spdk_io_channel_iter_get_ctx(i);
	struct nvme_bdev *nbdev = spdk_io_channel_iter_get_io_device(i);

	iter->i = i;
	iter->cpl(nbdev, iter->ctx, status);

	free(iter);
}

/*
 * [한국어]
 * nvme_bdev_for_each_channel - bdev_nvme.h §2 참조. 모든 스레드의 nvme_bdev 채널 순회.
 *
 * nvme_ctrlr 버전과 동일 패턴이지만 nvme_bdev에 대해 동작. 멀티패스 정책 변경,
 * io_path 캐시 무효화 등 bdev 단위 작업에 사용.
 */
void
nvme_bdev_for_each_channel(struct nvme_bdev *nbdev,
			   nvme_bdev_for_each_channel_msg fn, void *ctx,
			   nvme_bdev_for_each_channel_done cpl)
{
	struct nvme_bdev_channel_iter *iter;

	assert(nbdev != NULL && fn != NULL);

	iter = calloc(1, sizeof(struct nvme_bdev_channel_iter));
	if (iter == NULL) {
		NVME_BDEV_ERRLOG(nbdev, null_ctrlr, "Unable to allocate iterator\n");
		assert(false);
		return;
	}

	iter->fn = fn;
	iter->cpl = cpl;
	iter->ctx = ctx;

	spdk_for_each_channel(nbdev, nvme_bdev_each_channel_msg, iter,
			      nvme_bdev_each_channel_cpl);
}

/*
 * [한국어]
 * nvme_bdev_dump_trid_json - bdev_nvme.h §2 참조.
 * trid의 모든 비어있지 않은 필드(trtype/adrfam/traddr/trsvcid/subnqn)를 JSON으로 출력.
 * RPC 응답에서 컨트롤러 정보의 trid 부분 직렬화에 사용.
 */
void
nvme_bdev_dump_trid_json(const struct spdk_nvme_transport_id *trid, struct spdk_json_write_ctx *w)
{
	const char *trtype_str;
	const char *adrfam_str;

	trtype_str = spdk_nvme_transport_id_trtype_str(trid->trtype);
	if (trtype_str) {
		spdk_json_write_named_string(w, "trtype", trtype_str);
	}

	adrfam_str = spdk_nvme_transport_id_adrfam_str(trid->adrfam);
	if (adrfam_str) {
		spdk_json_write_named_string(w, "adrfam", adrfam_str);
	}

	if (trid->traddr[0] != '\0') {
		spdk_json_write_named_string(w, "traddr", trid->traddr);
	}

	if (trid->trsvcid[0] != '\0') {
		spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);
	}

	if (trid->subnqn[0] != '\0') {
		spdk_json_write_named_string(w, "subnqn", trid->subnqn);
	}
}

static void
nvme_bdev_ctrlr_delete(struct nvme_bdev_ctrlr *nbdev_ctrlr,
		       struct nvme_ctrlr *nvme_ctrlr)
{
	SPDK_DTRACE_PROBE1(bdev_nvme_ctrlr_delete, nvme_ctrlr->nbdev_ctrlr->name);

	assert(spdk_thread_is_app_thread(NULL));

	TAILQ_REMOVE(&nbdev_ctrlr->ctrlrs, nvme_ctrlr, tailq);
	if (!TAILQ_EMPTY(&nbdev_ctrlr->ctrlrs)) {
		return;
	}
	TAILQ_REMOVE(&g_nvme_bdev_ctrlrs, nbdev_ctrlr, tailq);

	assert(TAILQ_EMPTY(&nbdev_ctrlr->bdevs));

	free(nbdev_ctrlr->name);
	free(nbdev_ctrlr);
}

static void
bdev_nvme_fini_done(void)
{
	if (!g_bdev_nvme_module_finish || !TAILQ_EMPTY(&g_nvme_bdev_ctrlrs)) {
		return;
	}

	spdk_io_device_unregister(&g_nvme_bdev_ctrlrs, NULL);
	spdk_bdev_module_fini_done();
}

static void
_nvme_ctrlr_delete(struct nvme_ctrlr *nvme_ctrlr)
{
	struct spdk_nvme_path_id *path_id, *tmp_path;
	struct nvme_ns *ns, *tmp_ns;

	assert(spdk_thread_is_app_thread(NULL));

	free(nvme_ctrlr->copied_ana_desc);
	spdk_free(nvme_ctrlr->ana_log_page);

	if (nvme_ctrlr->opal_dev) {
		spdk_opal_dev_destruct(nvme_ctrlr->opal_dev);
		nvme_ctrlr->opal_dev = NULL;
	}

	if (nvme_ctrlr->nbdev_ctrlr) {
		nvme_bdev_ctrlr_delete(nvme_ctrlr->nbdev_ctrlr, nvme_ctrlr);
	}

	RB_FOREACH_SAFE(ns, nvme_ns_tree, &nvme_ctrlr->namespaces, tmp_ns) {
		RB_REMOVE(nvme_ns_tree, &nvme_ctrlr->namespaces, ns);
		nvme_ns_free(ns);
	}

	TAILQ_FOREACH_SAFE(path_id, &nvme_ctrlr->trids, link, tmp_path) {
		TAILQ_REMOVE(&nvme_ctrlr->trids, path_id, link);
		free(path_id);
	}

	pthread_mutex_destroy(&nvme_ctrlr->mutex);
	spdk_keyring_put_key(nvme_ctrlr->psk);
	spdk_keyring_put_key(nvme_ctrlr->dhchap_key);
	spdk_keyring_put_key(nvme_ctrlr->dhchap_ctrlr_key);
	free(nvme_ctrlr);
	bdev_nvme_fini_done();
}

/*
 * [한국어]
 * nvme_detach_poller - NVMe 컨트롤러 비동기 detach 진행을 폴링하는 poller 콜백
 *
 * @arg: SPDK_POLLER_REGISTER 시 등록한 컨텍스트. 여기서는 detach 대상 nvme_ctrlr.
 * @return: 항상 SPDK_POLLER_BUSY(=1). poller가 매번 의미 있는 일을 했다고 보고하여
 *          reactor의 idle 카운트 휴리스틱에 "바쁨"으로 집계되게 한다.
 *
 * 왜 필요한가: spdk_nvme_detach_async()는 PCIe 컨트롤러를 즉시 분리하지 않고
 * 비동기 절차(qpair 해제, admin 큐 정리, 디바이스 reset 등)를 시작한다. 그 진행을
 * 매 폴마다 spdk_nvme_detach_poll_async()로 한 스텝씩 밀어줘야 완료된다. 이 poller가
 * 그 펌핑 역할을 한다.
 *
 * 동작 단계:
 *   1) spdk_nvme_detach_poll_async()로 detach 상태머신을 한 스텝 진행.
 *   2) 반환값이 -EAGAIN이면 아직 진행 중 → 다음 폴까지 대기(아무 것도 정리 안 함).
 *   3) -EAGAIN이 아니면(0=완료 또는 그 외 에러) 더 폴링할 이유가 없으므로 poller를
 *      해제하고 _nvme_ctrlr_delete()로 nvme_ctrlr 자체를 최종 해제한다.
 *
 * 실행 컨텍스트: app thread(주로 main thread)에서 1000us 주기로 도는 SPDK poller.
 * nvme_ctrlr_delete()가 SPDK_POLLER_REGISTER로 등록한다. 단일 스레드에서만 돌므로
 * detach_ctx/reset_detach_poller 접근에 별도 락 불필요.
 *
 * 호출 체인:
 *   nvme_ctrlr_delete() → SPDK_POLLER_REGISTER(nvme_detach_poller)
 *     → [nvme_detach_poller] → spdk_nvme_detach_poll_async() / _nvme_ctrlr_delete()
 */
static int
nvme_detach_poller(void *arg)
{
	/* [한국어] poller 등록 시 넘긴 컨텍스트를 detach 대상 컨트롤러로 복원. */
	struct nvme_ctrlr *nvme_ctrlr = arg;
	int rc;

	/* [한국어] NVMe 드라이버의 비동기 detach 상태머신을 한 스텝 진행시킨다.
	 * detach_ctx는 spdk_nvme_detach_async()가 발급한 진행 컨텍스트 핸들.
	 * 반환: -EAGAIN = 아직 진행 중(다음 폴 필요), 0 = 완료, 그 외 = 에러. */
	rc = spdk_nvme_detach_poll_async(nvme_ctrlr->detach_ctx);
	/* [한국어] -EAGAIN이 아니면 detach가 끝났거나(0) 더 진행할 수 없는 상태이므로
	 * 정리 단계로 넘어간다. -EAGAIN이면 이 if를 건너뛰고 다음 폴을 기다린다. */
	if (rc != -EAGAIN) {
		/* [한국어] 이 poller 자신을 reactor에서 등록 해제(더 이상 폴링 불필요). */
		spdk_poller_unregister(&nvme_ctrlr->reset_detach_poller);
		/* [한국어] nvme_ctrlr 구조체와 부속 자원(mutex/key/메모리)을 최종 해제하고
		 * bdev_nvme_fini_done()까지 호출하는 종착 함수. */
		_nvme_ctrlr_delete(nvme_ctrlr);
	}

	/* [한국어] poller는 매 호출마다 "일을 했다"는 의미로 BUSY를 반환한다. */
	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * nvme_ctrlr_delete - nvme_ctrlr 파괴 절차의 본체: poller/interrupt 정리 후 비동기 detach 시작
 *
 * @nvme_ctrlr: 파괴할 컨트롤러. io_device unregister 콜백 경로를 통해 ref==0 + destruct
 *              상태가 확정된 뒤 들어온다(즉 더 이상 I/O가 없음).
 * @return: 없음. 실제 자원 해제는 detach 완료 후 nvme_detach_poller가 마무리한다.
 *
 * 왜 필요한가: 컨트롤러를 안전하게 제거하려면 (a) 이 컨트롤러에 걸린 각종 poller와
 * interrupt 핸들러를 먼저 끊고, (b) 하드웨어 detach(qpair 정리, reset 등)를 비동기로
 * 진행해야 한다. 이 함수는 (a)를 수행하고 (b)를 트리거한 뒤 곧장 반환한다 — 실제 완료는
 * nvme_detach_poller가 폴링으로 이어받는다.
 *
 * 동작 단계:
 *   1) reconnect 지연 타이머 poller 해제.
 *   2) interrupt mode면 등록된 인터럽트 핸들러 해제.
 *   3) adminq 타이머 poller 해제(driver가 detach 중 adminq를 직접 폴링하므로 먼저 끊음).
 *   4) detach 진행용 poller(nvme_detach_poller) 1000us 주기로 등록.
 *   5) spdk_nvme_detach_async()로 비동기 detach 시작.
 *   6) 4)/5) 중 실패하면 error 라벨로 점프 — detach 없이라도 구조체는 해제한다.
 *
 * 실행 컨텍스트: app thread. spdk_io_device_unregister()의 unregister_cb 경로
 * (nvme_ctrlr_unregister_cb)에서 호출되며, 이 시점에 채널이 모두 파괴되어 있다.
 *
 * 호출 체인:
 *   nvme_ctrlr_unregister_cb() → [nvme_ctrlr_delete]
 *     → spdk_nvme_detach_async() / SPDK_POLLER_REGISTER(nvme_detach_poller)
 */
static void
nvme_ctrlr_delete(struct nvme_ctrlr *nvme_ctrlr)
{
	int rc;

	/* [한국어] 재연결(reconnect) 대기용 지연 타이머 poller를 해제한다. 컨트롤러가
	 * 사라지므로 더 이상 재연결을 시도할 이유가 없다. */
	spdk_poller_unregister(&nvme_ctrlr->reconnect_delay_timer);

	/* [한국어] interrupt mode(폴링 대신 eventfd/MSI-X 인터럽트로 깨우는 모드)일 때만
	 * 등록된 인터럽트 핸들러가 존재한다. 해당 모드면 그 핸들러를 해제한다. */
	if (spdk_interrupt_mode_is_enabled()) {
		spdk_interrupt_unregister(&nvme_ctrlr->intr);
	}

	/* First, unregister the adminq poller, as the driver will poll adminq if necessary */
	/* [한국어] admin 큐 타이머 poller를 가장 먼저 해제한다. detach 도중에는 NVMe
	 * 드라이버 내부가 필요 시 adminq를 직접 폴링하므로, 우리 쪽 poller가 동시에
	 * 돌면 충돌한다. 그래서 우선 끊는다. */
	spdk_poller_unregister(&nvme_ctrlr->adminq_timer_poller);

	/* If we got here, the reset/detach poller cannot be active */
	/* [한국어] 여기 도달했다는 것은 reset이 진행 중이 아니라는 뜻이므로,
	 * reset과 공용으로 쓰는 reset_detach_poller 슬롯이 비어 있어야 한다(불변식 검증). */
	assert(nvme_ctrlr->reset_detach_poller == NULL);
	/* [한국어] detach 진행을 펌핑할 poller를 1000us(=1ms) 주기로 등록한다.
	 * 이후 nvme_detach_poller가 spdk_nvme_detach_poll_async()를 반복 호출하며 완료를 기다린다. */
	nvme_ctrlr->reset_detach_poller = SPDK_POLLER_REGISTER(nvme_detach_poller,
					  nvme_ctrlr, 1000);
	/* [한국어] poller 등록 실패(메모리 부족 등) 시 detach를 폴링할 수단이 없으므로
	 * error 경로로 가서 강제 정리한다. */
	if (nvme_ctrlr->reset_detach_poller == NULL) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "Failed to register detach poller\n");
		goto error;
	}

	/* [한국어] NVMe 드라이버에 비동기 detach를 요청한다. 진행 컨텍스트가
	 * detach_ctx에 채워지고, 위 poller가 그 핸들로 진행을 폴링한다.
	 * rc==0이면 detach 절차가 정상 시작됨. */
	rc = spdk_nvme_detach_async(nvme_ctrlr->ctrlr, &nvme_ctrlr->detach_ctx);
	/* [한국어] detach 시작 자체가 실패하면 폴링할 대상이 없으므로 error 경로로. */
	if (rc != 0) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "Failed to detach the NVMe controller\n");
		goto error;
	}

	/* [한국어] 정상 경로: detach가 시작되었고, 나머지는 poller가 처리하므로 반환. */
	return;
error:
	/* We don't have a good way to handle errors here, so just do what we can and delete the
	 * controller without detaching the underlying NVMe device.
	 */
	/* [한국어] 에러 복구 경로: 여기서는 마땅한 회복 수단이 없으므로, 등록되었을 수도 있는
	 * detach poller를 해제하고(하드웨어 detach 없이) 구조체만이라도 해제한다.
	 * 하드웨어를 detach하지 못하므로 디바이스가 깨끗하지 않은 상태로 남을 수 있다. */
	spdk_poller_unregister(&nvme_ctrlr->reset_detach_poller);
	_nvme_ctrlr_delete(nvme_ctrlr);
}

/*
 * [한국어]
 * nvme_ctrlr_unregister_cb - io_device unregister 완료 콜백, 실제 파괴를 시작
 *
 * @io_device: spdk_io_device_unregister()에 넘긴 io_device 포인터. 여기서는
 *             nvme_ctrlr 자신(컨트롤러를 io_device로 등록했었음).
 * @return: 없음.
 *
 * 왜 필요한가: spdk_io_device_unregister()는 등록된 모든 채널이 파괴되기를 기다린 뒤
 * 이 콜백을 호출한다. 즉 이 콜백이 불릴 때면 컨트롤러의 채널이 전부 사라진 안전한 시점이다.
 * 그제서야 컨트롤러 본체 파괴(nvme_ctrlr_delete)를 시작한다.
 *
 * 실행 컨텍스트: app thread. SPDK io_device 프레임워크가 채널 파괴 완료 후 호출.
 *
 * 호출 체인:
 *   nvme_ctrlr_unregister() → spdk_io_device_unregister(..., nvme_ctrlr_unregister_cb)
 *     → [nvme_ctrlr_unregister_cb] → nvme_ctrlr_delete()
 */
static void
nvme_ctrlr_unregister_cb(void *io_device)
{
	/* [한국어] io_device 포인터를 nvme_ctrlr로 복원(컨트롤러를 io_device로 등록했었음). */
	struct nvme_ctrlr *nvme_ctrlr = io_device;

	/* [한국어] 채널이 모두 정리된 안전한 시점이므로 컨트롤러 파괴 본체로 진입. */
	nvme_ctrlr_delete(nvme_ctrlr);
}

/*
 * [한국어]
 * nvme_ctrlr_unregister - 컨트롤러 io_device 등록 해제를 트리거(파괴 절차의 진입점)
 *
 * @ctx: 메시지 컨텍스트. nvme_ctrlr 포인터.
 * @return: 없음.
 *
 * 왜 필요한가: 컨트롤러 파괴는 반드시 app thread에서 시작되어야 한다(io_device 등록을
 * app thread가 소유). nvme_ctrlr_put_ref_ext()는 ref가 0이 되고 파괴 조건이 충족되면
 * spdk_thread_send_msg(app_thread, nvme_ctrlr_unregister, ...)로 이 함수를 app thread에
 * 디스패치한다. 이 함수는 io_device unregister를 호출해 채널 파괴 → unregister_cb 체인을 연다.
 *
 * 실행 컨텍스트: app thread(spdk_thread_send_msg를 통해 진입).
 *
 * 호출 체인:
 *   nvme_ctrlr_put_ref_ext() → spdk_thread_send_msg(app_thread, nvme_ctrlr_unregister)
 *     → [nvme_ctrlr_unregister] → spdk_io_device_unregister() → nvme_ctrlr_unregister_cb()
 */
static void
nvme_ctrlr_unregister(void *ctx)
{
	/* [한국어] 메시지 컨텍스트를 nvme_ctrlr로 복원. */
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	/* [한국어] 컨트롤러를 io_device 레지스트리에서 제거 요청. 프레임워크가 모든 채널을
	 * 파괴한 뒤 nvme_ctrlr_unregister_cb를 호출하도록 콜백을 등록한다. */
	spdk_io_device_unregister(nvme_ctrlr, nvme_ctrlr_unregister_cb);
}

/*
 * [한국어]
 * nvme_ctrlr_can_be_unregistered - 컨트롤러를 지금 unregister(파괴)해도 안전한지 판정
 *
 * @nvme_ctrlr: 판정 대상. 호출자가 mutex를 잡은 상태에서 들어와야 한다(아래 동기화 참고).
 * @return: true = 파괴 가능(destruct 요청됨 + ref 0 + reset 미진행), false = 아직 불가.
 *
 * 왜 필요한가: 컨트롤러는 마지막 참조(ref)가 사라질 때 자동 파괴되는데, 단순히 ref==0만으로는
 * 부족하다. (1) 누군가 파괴를 명시적으로 요청(destruct)했어야 하고, (2) reset 같은 진행 중
 * 작업이 없어야 한다. 이 세 조건을 한 곳에서 검사해 race를 막는다.
 *
 * 동작 단계:
 *   1) destruct 플래그가 안 서 있으면 파괴 요청 자체가 없으므로 false.
 *   2) ref > 0이면 아직 사용 중이므로 false.
 *   3) resetting 중이면 reset이 완료될 때 다시 판정되어야 하므로 지금은 false.
 *   4) 모두 통과하면 ana_log_page_updating / io_path_cache_clearing 같은 임시 플래그가
 *      절대 서 있지 않음을 assert로 보장(아래 동기화 주석 참조) 후 true.
 *
 * 실행 컨텍스트: 호출자(nvme_ctrlr_put_ref_ext)가 nvme_ctrlr->mutex를 잡은 채 호출.
 *
 * 동기화: ana_log_page_updating/io_path_cache_clearing 같은 플래그는 항상 ref를 get한 뒤
 * 세우고 ref를 put하기 전에 지운다. 따라서 ref==0이 확인되면 이 플래그들이 false임이
 * 보장되므로 위 3가지 검사만으로 충분하다(assert로 그 불변식을 문서화·검증).
 *
 * 호출 체인:
 *   nvme_ctrlr_put_ref_ext() → [nvme_ctrlr_can_be_unregistered]
 */
static bool
nvme_ctrlr_can_be_unregistered(struct nvme_ctrlr *nvme_ctrlr)
{
	/* [한국어] 파괴(destruct)가 요청되지 않았으면 ref가 0이어도 살아있는 컨트롤러이므로
	 * 파괴 불가. (예: 일시적으로 아무도 안 쓰지만 계속 존재해야 하는 경우) */
	if (!nvme_ctrlr->destruct) {
		return false;
	}

	/* [한국어] 아직 참조가 남아 있으면(채널/진행 중 작업) 파괴 불가. */
	if (nvme_ctrlr->ref > 0) {
		return false;
	}

	/* [한국어] reset이 진행 중이면 reset 완료 시점에 다시 put_ref가 불려 재판정되므로
	 * 지금은 파괴를 보류한다. */
	if (nvme_ctrlr->resetting) {
		return false;
	}

	/* Flags are set after ref get and cleared before ref put, so the above check is sufficient. */
	/* [한국어] 위 불변식(플래그는 ref get 후 set, ref put 전 clear) 하에서, ref==0이면
	 * 이 두 임시 작업 플래그는 반드시 false여야 한다. 그렇지 않으면 버그이므로 assert로 검증. */
	assert(!nvme_ctrlr->ana_log_page_updating);
	assert(!nvme_ctrlr->io_path_cache_clearing);
	/* [한국어] 모든 조건 충족 → 지금 안전하게 unregister/파괴 가능. */
	return true;
}

/* Invokes cb_fn under the ctrlr’s lock but only if not scheduled to unregister. */
/*
 * [한국어]
 * nvme_ctrlr_put_ref_ext - 컨트롤러 참조 카운트 1 감소, 0이 되어 파괴 가능하면 파괴 트리거
 *
 * @nvme_ctrlr: 참조를 놓을 컨트롤러.
 * @cb_fn: 선택적 콜백. 참조를 줄였지만 아직 파괴되지 않을 때(=계속 살아있을 때) mutex를
 *         잡은 채로 호출된다. NULL이면 호출 생략. (파괴로 넘어가는 경우엔 호출되지 않음)
 * @return: 없음.
 *
 * 왜 필요한가: 컨트롤러는 ref 카운팅으로 수명을 관리한다. 마지막 참조가 놓이고 파괴 조건이
 * 충족되면 자동으로 파괴 절차를 시작해야 한다. cb_fn은 "참조는 줄었지만 살아있을 때만"
 * 어떤 후처리를 mutex 보호 하에 원자적으로 실행하기 위한 훅이다.
 *
 * 동작 단계:
 *   1) mutex 잡고 DTrace probe로 (이름, 현재 ref) 기록.
 *   2) ref가 양수임을 assert(underflow 방지)한 뒤 1 감소.
 *   3) 파괴 불가 상태면: cb_fn이 있으면 호출하고, 락 풀고 반환(살려둠).
 *   4) 파괴 가능 상태면: 락 풀고 app thread로 nvme_ctrlr_unregister 메시지를 보내 파괴 시작.
 *
 * 실행 컨텍스트: 임의의 SPDK thread에서 호출 가능. 파괴는 반드시 app thread에서 해야 하므로
 * spdk_thread_send_msg로 디스패치한다(cross-thread 안전).
 *
 * 동기화: nvme_ctrlr->mutex(pthread_mutex)로 ref 감소와 파괴 판정을 원자적으로 묶는다.
 * 파괴 메시지를 보내기 전 반드시 락을 해제한다(메시지 핸들러가 같은 락을 잡을 수 있어 deadlock 방지).
 *
 * 호출 체인:
 *   nvme_ctrlr_put_ref() / 각종 release 경로 → [nvme_ctrlr_put_ref_ext]
 *     → cb_fn() 또는 spdk_thread_send_msg(app_thread, nvme_ctrlr_unregister)
 */
static void
nvme_ctrlr_put_ref_ext(struct nvme_ctrlr *nvme_ctrlr, nvme_ctrlr_put_ref_cb cb_fn)
{
	/* [한국어] ref 감소와 파괴 판정을 원자적으로 처리하기 위해 컨트롤러 mutex 획득. */
	pthread_mutex_lock(&nvme_ctrlr->mutex);
	/* [한국어] DTrace 정적 프로브: 컨트롤러 이름과 (감소 전) ref 값을 추적 도구에 노출. */
	SPDK_DTRACE_PROBE2(bdev_nvme_ctrlr_release, nvme_ctrlr->nbdev_ctrlr->name, nvme_ctrlr->ref);

	/* [한국어] put이 get보다 많이 불리면 ref가 음수로 내려가는 버그이므로 양수임을 검증. */
	assert(nvme_ctrlr->ref > 0);
	/* [한국어] 참조 카운트 1 감소. mutex 보호 하의 단순 정수 연산. */
	nvme_ctrlr->ref--;

	/* [한국어] 감소 후에도 파괴 조건(destruct + ref==0 + !resetting)을 만족하지 못하면
	 * 컨트롤러는 계속 살아있어야 한다. */
	if (!nvme_ctrlr_can_be_unregistered(nvme_ctrlr)) {
		/* [한국어] 살아있는 상태에서만 실행해야 하는 후처리 콜백이 있으면 락을 잡은 채 호출.
		 * (예: 다음 작업 스케줄링 등 ref/상태와 원자적이어야 하는 동작) */
		if (cb_fn) {
			cb_fn(nvme_ctrlr);
		}

		/* [한국어] 락 해제 후 반환. 파괴로 넘어가지 않음. */
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return;
	}

	/* [한국어] 파괴 가능 상태: 메시지 핸들러와의 deadlock 방지를 위해 먼저 락 해제. */
	pthread_mutex_unlock(&nvme_ctrlr->mutex);
	/* [한국어] 파괴는 app thread 소유 작업이므로, app thread로 nvme_ctrlr_unregister를
	 * 비동기 메시지로 디스패치한다(lockless 메시지 패싱). */
	spdk_thread_send_msg(spdk_thread_get_app_thread(), nvme_ctrlr_unregister, nvme_ctrlr);
}

/*
 * [한국어]
 * nvme_ctrlr_put_ref - cb_fn 없이 참조를 놓는 단순 래퍼
 *
 * @nvme_ctrlr: 참조를 놓을 컨트롤러.
 * @return: 없음.
 *
 * 후처리 콜백이 필요 없는 일반적인 release 경로에서 사용한다. 내부적으로 cb_fn=NULL로
 * nvme_ctrlr_put_ref_ext()를 호출할 뿐이다.
 *
 * 호출 체인:
 *   (각종 release 경로) → [nvme_ctrlr_put_ref] → nvme_ctrlr_put_ref_ext(.., NULL)
 */
static void
nvme_ctrlr_put_ref(struct nvme_ctrlr *nvme_ctrlr)
{
	/* [한국어] 후처리 콜백 없이 참조만 1 감소시키는 일반 경로. */
	nvme_ctrlr_put_ref_ext(nvme_ctrlr, NULL);
}

/*
 * [한국어]
 * nvme_ctrlr_get_ref - 컨트롤러 참조 카운트를 1 증가시켜 수명을 연장
 *
 * @nvme_ctrlr: 참조를 얻을 컨트롤러. 호출 시점에 이미 ref>0(살아있음)이어야 한다.
 * @return: 없음.
 *
 * 왜 필요한가: 컨트롤러를 사용하기 시작하는 주체(채널 생성, reset, ANA 갱신 등)가 사용 동안
 * 컨트롤러가 파괴되지 않도록 참조를 올린다. 사용이 끝나면 짝이 되는 put_ref로 내린다.
 *
 * 실행 컨텍스트: 임의 SPDK thread. mutex로 보호되므로 cross-thread 안전.
 *
 * 동기화: nvme_ctrlr->mutex로 ref 증가를 원자화. assert(ref>0)는 "0에서 부활"하는
 * use-after-free성 버그를 막는다(이미 죽은 컨트롤러를 되살릴 수 없음).
 *
 * 호출 체인:
 *   (채널 생성/reset/ANA 등) → [nvme_ctrlr_get_ref]
 */
static void
nvme_ctrlr_get_ref(struct nvme_ctrlr *nvme_ctrlr)
{
	/* [한국어] ref 증가를 원자화하기 위해 mutex 획득. */
	pthread_mutex_lock(&nvme_ctrlr->mutex);
	/* [한국어] 이미 살아있는(ref>0) 컨트롤러만 참조를 추가할 수 있다(0에서 부활 금지). */
	assert(nvme_ctrlr->ref > 0);
	/* [한국어] 참조 카운트 1 증가 — 사용 동안 파괴를 막는다. */
	nvme_ctrlr->ref++;
	/* [한국어] mutex 해제. */
	pthread_mutex_unlock(&nvme_ctrlr->mutex);
}

/*
 * [한국어]
 * bdev_nvme_clear_current_io_path - 채널의 캐시된 현재 io_path와 라운드로빈 카운터를 무효화
 *
 * @nbdev_ch: 멀티패스 bdev 채널(코어/스레드별 I/O 채널 컨텍스트).
 * @return: 없음.
 *
 * 왜 필요한가: nbdev_ch->current_io_path는 다음 I/O를 어느 경로로 보낼지 매번 재계산하지
 * 않으려고 캐싱해 둔 "현재 선택된 경로"다. io_path 추가/삭제, ANA 상태 변화, 경로 장애 등으로
 * 그 선택이 더 이상 유효하지 않을 때 캐시를 비워, 다음 I/O 발행 시 경로를 새로 고르게 한다.
 *
 * 실행 컨텍스트: 해당 채널을 소유한 SPDK thread(reactor). 채널은 스레드에 고정(affinity)
 * 되어 단일 스레드에서만 접근하므로 락 없이 안전(lockless).
 *
 * 호출 체인:
 *   _bdev_nvme_add_io_path() / _bdev_nvme_delete_io_path() 등 → [bdev_nvme_clear_current_io_path]
 */
static void
bdev_nvme_clear_current_io_path(struct nvme_bdev_channel *nbdev_ch)
{
	/* [한국어] 캐시된 현재 경로 포인터를 비워 다음 I/O 때 경로를 재선택하도록 강제. */
	nbdev_ch->current_io_path = NULL;
	/* [한국어] 라운드로빈 선택 카운터도 0으로 리셋 — 경로 집합이 바뀌면 RR 위치도 무의미. */
	nbdev_ch->rr_counter = 0;
}

/*
 * [한국어]
 * _bdev_nvme_get_io_path - 채널의 io_path 리스트에서 특정 namespace에 대응하는 경로를 검색
 *
 * @nbdev_ch: 멀티패스 bdev 채널.
 * @nvme_ns: 찾고자 하는 namespace(컨트롤러 경로 식별자 역할).
 * @return: 해당 nvme_ns를 가리키는 nvme_io_path 포인터. 없으면 NULL.
 *
 * 왜 필요한가: 하나의 논리 bdev(namespace)에 여러 물리 경로(컨트롤러)가 연결될 수 있다.
 * 특정 namespace에 대한 경로가 이미 채널에 존재하는지 확인할 때 사용한다(중복 추가/삭제 판단).
 *
 * 동작: io_path_list를 선형 순회하며 io_path->nvme_ns == nvme_ns인 항목을 찾는다.
 * STAILQ_FOREACH가 끝까지 돌면 io_path는 NULL이 되어 "없음"을 자연스럽게 반환한다.
 *
 * 실행 컨텍스트: 채널 소유 스레드. 단일 스레드 접근이므로 lockless.
 *
 * 호출 체인:
 *   bdev_nvme_add_io_path()/delete 경로 등 → [_bdev_nvme_get_io_path]
 */
static struct nvme_io_path *
_bdev_nvme_get_io_path(struct nvme_bdev_channel *nbdev_ch, struct nvme_ns *nvme_ns)
{
	struct nvme_io_path *io_path;

	/* [한국어] 채널의 모든 io_path를 선형 순회(STAILQ: singly-linked tail queue). */
	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		/* [한국어] 찾는 namespace를 가리키는 경로를 만나면 루프 종료(io_path가 결과). */
		if (io_path->nvme_ns == nvme_ns) {
			break;
		}
	}

	/* [한국어] 일치 항목을 찾았으면 그 포인터, 끝까지 못 찾았으면 NULL을 반환. */
	return io_path;
}

/*
 * [한국어]
 * nvme_io_path_alloc - 새 nvme_io_path 객체를 힙에서 할당하고 (옵션) per-path 통계 버퍼까지 준비한다.
 *
 * @return: 성공 시 0으로 초기화된 io_path 포인터, 실패 시 NULL.
 *
 * nvme_io_path는 "특정 IO 채널(=특정 스레드)에서, 특정 nvme_ns로 가는 하나의 경로"를
 * 나타내는 멀티패스 핵심 자료구조다. 한 namespace에 여러 컨트롤러(path)가 붙은 경우
 * 채널마다 path 개수만큼 io_path가 생긴다. 이 함수는 그 객체 한 개를 만든다.
 * g_opts.io_path_stat가 켜져 있으면 경로별 IO 통계(spdk_bdev_io_stat)도 같이 할당하고
 * MAXMIN(min/max latency 추적) 모드로 리셋한다.
 * 실행 컨텍스트: bdev 채널 생성 콜백(해당 채널 스레드)에서 호출되는 lockless 경로.
 *
 * 호출 체인:
 *   _bdev_nvme_add_io_path → [nvme_io_path_alloc] → calloc / spdk_bdev_reset_io_stat
 */
static struct nvme_io_path *
nvme_io_path_alloc(void)
{
	struct nvme_io_path *io_path;        /* [한국어] 새로 만들 경로 객체 포인터. */

	/* [한국어] calloc으로 0초기화 할당 — 모든 포인터/플래그가 NULL/0에서 시작하도록. */
	io_path = calloc(1, sizeof(*io_path));
	if (io_path == NULL) {
		/* [한국어] 메모리 부족: 경로 생성 불가 → 호출자가 -ENOMEM으로 처리. */
		SPDK_ERRLOG("Failed to alloc io_path.\n");
		return NULL;
	}

	/* [한국어] 경로별 통계 수집 옵션이 켜진 경우에만 별도 stat 버퍼를 추가 할당. */
	if (g_opts.io_path_stat) {
		io_path->stat = calloc(1, sizeof(struct spdk_bdev_io_stat));
		if (io_path->stat == NULL) {
			/* [한국어] stat 할당 실패 시 이미 잡은 io_path를 되돌려야 누수가 없다. */
			free(io_path);
			SPDK_ERRLOG("Failed to alloc io_path stat.\n");
			return NULL;
		}
		/* [한국어] 통계 카운터를 0으로, min/max latency 추적(MAXMIN)을 활성화. */
		spdk_bdev_reset_io_stat(io_path->stat, SPDK_BDEV_RESET_STAT_MAXMIN);
	}

	/* [한국어] 준비된 경로 객체 반환 — 호출자가 nbdev_ch/qpair 리스트에 연결한다. */
	return io_path;
}

/*
 * [한국어]
 * nvme_io_path_free - nvme_io_path 객체와 부속 통계 버퍼를 해제한다.
 *
 * @io_path: 해제할 경로 객체. 어느 리스트에도 더 이상 연결되어 있지 않아야 안전.
 *
 * stat 버퍼는 NULL일 수 있으나 free(NULL)은 무해하므로 무조건 호출한다.
 * 주의: 경로 삭제(_bdev_nvme_delete_io_path)에서 바로 free하지 않고, 연관 qpair가
 * 해제되는 시점까지 미뤄 free하는 정책이 있다(use-after-free 방지). 이 함수는 그
 * 안전 시점에 호출되는 최종 해제기다.
 *
 * 호출 체인:
 *   qpair 해제 경로 / add 실패 롤백 → [nvme_io_path_free] → free
 */
static void
nvme_io_path_free(struct nvme_io_path *io_path)
{
	free(io_path->stat);    /* [한국어] 경로별 통계 버퍼 해제(없으면 NULL이라 무해). */
	free(io_path);          /* [한국어] 경로 객체 본체 해제. */
}

/*
 * [한국어]
 * _bdev_nvme_add_io_path - 주어진 nvme_ns로 가는 새 경로를 이 bdev 채널에 연결한다.
 *
 * @nbdev_ch: 경로를 추가받을 nvme_bdev 채널(특정 스레드 소유).
 * @nvme_ns: 이 경로가 향하는 namespace(특정 컨트롤러에 속함).
 * @return: 성공 0, 메모리 부족 시 -ENOMEM.
 *
 * 멀티패스 토폴로지: 하나의 nvme_bdev(논리 디스크)는 여러 nvme_ns(=여러 컨트롤러
 * 경로)를 가질 수 있다. 채널이 생성되거나 새 path가 attach될 때, 이 함수가 채널 안에
 * io_path를 만들어 (1) 해당 컨트롤러의 IO 채널을 잡고 → (2) 그 채널의 qpair를 경로에
 * 연결하고 → (3) qpair의 io_path_list와 채널의 io_path_list 양쪽에 등록한다.
 * 두 리스트에 동시에 거는 이유: qpair 단위(disconnect/free)와 채널 단위(path 선택)에서
 * 각각 역참조가 필요하기 때문이다.
 * 실행 컨텍스트: nbdev_ch를 소유한 스레드. spdk_get_io_channel은 현재 스레드의 채널을 반환.
 *
 * 호출 체인:
 *   bdev_nvme_create_bdev_channel_cb / path 추가 경로 → [_bdev_nvme_add_io_path]
 *     → spdk_get_io_channel → TAILQ/STAILQ_INSERT → bdev_nvme_clear_current_io_path
 */
static int
_bdev_nvme_add_io_path(struct nvme_bdev_channel *nbdev_ch, struct nvme_ns *nvme_ns)
{
	struct nvme_io_path *io_path;          /* [한국어] 새로 만들 경로 객체. */
	struct spdk_io_channel *ch;            /* [한국어] 컨트롤러 io_device에서 얻은 IO 채널. */
	struct nvme_ctrlr_channel *ctrlr_ch;   /* [한국어] ch의 컨텍스트(컨트롤러 채널). */
	struct nvme_qpair *nvme_qpair;         /* [한국어] 이 채널/스레드가 소유한 NVMe qpair. */

	/* [한국어] 경로 객체 할당(통계 포함 가능). 실패하면 즉시 -ENOMEM. */
	io_path = nvme_io_path_alloc();
	if (io_path == NULL) {
		return -ENOMEM;
	}

	/* [한국어] 이 경로가 향하는 namespace 기록 — IO 발행 시 nvme_ns->ns로 명령 빌드. */
	io_path->nvme_ns = nvme_ns;

	/* [한국어] namespace가 속한 컨트롤러(io_device)에 대한 현재 스레드의 IO 채널 획득.
	 * 이 호출이 컨트롤러 채널을 1 참조하므로, 경로 삭제 시 짝맞춰 put 해야 한다. */
	ch = spdk_get_io_channel(nvme_ns->ctrlr);
	if (ch == NULL) {
		/* [한국어] 채널 할당 실패: 이미 잡은 io_path를 되돌리고 -ENOMEM. */
		nvme_io_path_free(io_path);
		NVME_NS_ERRLOG(nvme_ns, "Failed to alloc io_channel.\n");
		return -ENOMEM;
	}

	/* [한국어] 채널 컨텍스트를 컨트롤러 채널 구조체로 해석. */
	ctrlr_ch = spdk_io_channel_get_ctx(ch);

	/* [한국어] 컨트롤러 채널이 보유한 qpair를 경로에 연결. 채널 생성 시 qpair가 만들어졌어야 함. */
	nvme_qpair = ctrlr_ch->qpair;
	assert(nvme_qpair != NULL);

	/* [한국어] 경로 → qpair 역참조 설정 + qpair의 io_path_list에 등록(qpair 해제 시 경로 정리용). */
	io_path->qpair = nvme_qpair;
	TAILQ_INSERT_TAIL(&nvme_qpair->io_path_list, io_path, tailq);

	/* [한국어] 경로 → 채널 역참조 + 채널의 io_path_list에 등록(path 선택 순회 대상). */
	io_path->nbdev_ch = nbdev_ch;
	STAILQ_INSERT_TAIL(&nbdev_ch->io_path_list, io_path, stailq);

	/* [한국어] 경로 집합이 바뀌었으므로 캐시된 current_io_path 무효화 → 다음 IO 때 재선택. */
	bdev_nvme_clear_current_io_path(nbdev_ch);

	return 0;     /* [한국어] 경로 추가 성공. */
}

/*
 * [한국어]
 * bdev_nvme_clear_retry_io_path - 삭제될 경로를 참조하는 재시도 대기 IO들의 경로 캐시를 끊는다.
 *
 * @nbdev_ch: 재시도 큐(retry_io_list)를 가진 bdev 채널.
 * @io_path: 곧 삭제될 경로 — 이 경로를 가리키는 모든 대기 IO의 io_path를 NULL로.
 *
 * 재시도 대기 중인 bdev_nvme_io는 직전에 시도한 io_path를 기억해 둔다(다음에 다른
 * 경로를 고르기 위한 힌트). 그 경로가 삭제되면 dangling 포인터가 되므로, 미리
 * NULL로 비워 다음 재시도 때 경로를 처음부터 재선택하도록 한다(use-after-free 방지).
 * 실행 컨텍스트: nbdev_ch 소유 스레드. 재시도 큐도 같은 스레드에서만 다뤄져 lockless.
 *
 * 호출 체인:
 *   _bdev_nvme_delete_io_path → [bdev_nvme_clear_retry_io_path]
 */
static void
bdev_nvme_clear_retry_io_path(struct nvme_bdev_channel *nbdev_ch,
			      struct nvme_io_path *io_path)
{
	struct nvme_bdev_io *bio;    /* [한국어] 재시도 큐를 순회할 IO 객체. */

	/* [한국어] 채널의 모든 재시도 대기 IO를 선형 순회. */
	TAILQ_FOREACH(bio, &nbdev_ch->retry_io_list, retry_link) {
		/* [한국어] 이 IO가 삭제될 경로를 기억하고 있으면 그 참조를 끊는다. */
		if (bio->io_path == io_path) {
			bio->io_path = NULL;
		}
	}
}

/*
 * [한국어]
 * _bdev_nvme_delete_io_path - 채널에서 한 경로를 떼어내고 컨트롤러 채널 참조를 반납한다.
 *
 * @nbdev_ch: 경로가 속한 bdev 채널.
 * @io_path: 제거할 경로.
 *
 * 핵심 주의: 경로를 채널 리스트에서 빼고 컨트롤러 IO 채널을 put 하지만, io_path 객체
 * 자체는 여기서 free 하지 않는다. 이미 발행된 IO가 완료되면서 io_path->stat을 갱신할
 * 수 있어, 지금 free 하면 use-after-free가 난다. 그래서 free는 연관 qpair가 해제되는
 * 시점(모든 IO 완료가 보장된 때)으로 미룬다. qpair의 io_path_list에는 여전히 남겨둔다.
 * 실행 컨텍스트: nbdev_ch 소유 스레드.
 *
 * 호출 체인:
 *   _bdev_nvme_delete_io_paths / path 제거 경로 → [_bdev_nvme_delete_io_path]
 *     → spdk_put_io_channel
 */
static void
_bdev_nvme_delete_io_path(struct nvme_bdev_channel *nbdev_ch, struct nvme_io_path *io_path)
{
	struct spdk_io_channel *ch;            /* [한국어] 반납할 컨트롤러 IO 채널 핸들. */
	struct nvme_qpair *nvme_qpair;         /* [한국어] 경로가 쓰던 qpair. */
	struct nvme_ctrlr_channel *ctrlr_ch;   /* [한국어] qpair가 속한 컨트롤러 채널. */

	/* [한국어] 경로가 가리키던 qpair 확보(채널 참조 반납 대상 추적용). */
	nvme_qpair = io_path->qpair;
	assert(nvme_qpair != NULL);

	/* [한국어] 캐시된 현재 경로/재시도 IO의 경로 참조를 모두 무효화(dangling 방지). */
	bdev_nvme_clear_current_io_path(nbdev_ch);
	bdev_nvme_clear_retry_io_path(nbdev_ch, io_path);

	/* [한국어] 채널의 경로 리스트에서만 제거(qpair 리스트는 남겨 둠 → 지연 free). */
	STAILQ_REMOVE(&nbdev_ch->io_path_list, io_path, nvme_io_path, stailq);
	io_path->nbdev_ch = NULL;     /* [한국어] 채널 역참조 끊기. */

	/* [한국어] qpair → 컨트롤러 채널 → io_channel 핸들로 거슬러 올라가 add 시 잡은 참조를 반납. */
	ctrlr_ch = nvme_qpair->ctrlr_ch;
	assert(ctrlr_ch != NULL);

	ch = spdk_io_channel_from_ctx(ctrlr_ch);
	spdk_put_io_channel(ch);      /* [한국어] _add에서 spdk_get_io_channel 한 것과 짝맞춤. */

	/* After an io_path is removed, I/Os submitted to it may complete and update statistics
	 * of the io_path. To avoid heap-use-after-free error from this case, do not free the
	 * io_path here but free the io_path when the associated qpair is freed. It is ensured
	 * that all I/Os submitted to the io_path are completed when the associated qpair is freed.
	 */
	/* [한국어] (위 영어 주석) 경로 제거 후에도 in-flight IO가 stat을 갱신할 수 있어,
	 * free는 qpair 해제 시점으로 지연한다 — 그때는 모든 IO 완료가 보장된다. */
}

/*
 * [한국어]
 * _bdev_nvme_delete_io_paths - 채널의 모든 경로를 한꺼번에 제거한다(채널 파괴 시).
 *
 * @nbdev_ch: 모든 경로를 떼어낼 bdev 채널.
 *
 * 채널이 파괴되거나, 경로 추가 중 실패해 롤백할 때 호출된다. _SAFE 변형을 쓰는 이유는
 * 순회 중 현재 노드를 리스트에서 제거하기 때문(다음 포인터를 미리 보관).
 * 실행 컨텍스트: nbdev_ch 소유 스레드.
 *
 * 호출 체인:
 *   bdev_nvme_destroy_bdev_channel_cb / add 실패 롤백 → [_bdev_nvme_delete_io_paths]
 *     → _bdev_nvme_delete_io_path
 */
static void
_bdev_nvme_delete_io_paths(struct nvme_bdev_channel *nbdev_ch)
{
	struct nvme_io_path *io_path, *tmp_io_path;    /* [한국어] 순회 커서와 다음 노드 보관용. */

	/* [한국어] 제거-안전 순회: 각 경로를 차례로 채널에서 떼어낸다. */
	STAILQ_FOREACH_SAFE(io_path, &nbdev_ch->io_path_list, stailq, tmp_io_path) {
		_bdev_nvme_delete_io_path(nbdev_ch, io_path);
	}
}

/*
 * [한국어]
 * bdev_nvme_create_bdev_channel_cb - nvme_bdev의 per-thread IO 채널을 초기화하는 콜백.
 *
 * @io_device: spdk_io_device_register에 등록된 nvme_bdev 포인터.
 * @ctx_buf: SPDK가 채널마다 할당해 주는 컨텍스트 메모리(= nvme_bdev_channel).
 * @return: 성공 0, 경로 생성 실패 시 음수(채널 생성 자체가 실패).
 *
 * spdk_bdev_get_io_channel()이 처음 이 스레드에서 호출될 때 SPDK 프레임워크가 채널
 * 컨텍스트를 만들고 이 콜백을 부른다. 여기서 채널의 경로 리스트/재시도 큐를 초기화하고,
 * nvme_bdev에 속한 모든 namespace 각각에 대해 io_path를 생성한다. 멀티패스 정책 필드
 * (mp_policy/mp_selector/rr_min_io)는 부모 nvme_bdev에서 채널로 복사해, IO 경로 선택을
 * 채널-로컬(lockless)로 수행할 수 있게 한다.
 * 실행 컨텍스트: 이 채널을 만드는 스레드. nbdev->mutex로 nvme_ns_list 순회를 보호
 * (path attach/detach가 다른 스레드에서 리스트를 변경할 수 있으므로).
 *
 * 호출 체인:
 *   spdk_bdev_get_io_channel → SPDK 코어 → [bdev_nvme_create_bdev_channel_cb]
 *     → _bdev_nvme_add_io_path (namespace 수만큼)
 */
static int
bdev_nvme_create_bdev_channel_cb(void *io_device, void *ctx_buf)
{
	struct nvme_bdev_channel *nbdev_ch = ctx_buf;     /* [한국어] 초기화할 채널 컨텍스트. */
	struct nvme_bdev *nbdev = io_device;              /* [한국어] 이 채널이 속한 논리 bdev. */
	struct nvme_ns *nvme_ns;                          /* [한국어] 순회용 namespace 커서. */
	int rc;                                           /* [한국어] 경로 추가 결과 코드. */

	/* [한국어] 채널-로컬 자료구조 초기화: 활성 경로 리스트와 재시도 대기 큐. */
	STAILQ_INIT(&nbdev_ch->io_path_list);
	TAILQ_INIT(&nbdev_ch->retry_io_list);

	/* [한국어] namespace 리스트와 멀티패스 정책 필드를 읽는 동안 변경되지 않도록 락. */
	pthread_mutex_lock(&nbdev->mutex);

	/* [한국어] 멀티패스 정책을 채널로 스냅샷 — 이후 경로 선택은 락 없이 채널 값으로 수행. */
	nbdev_ch->mp_policy = nbdev->mp_policy;       /* [한국어] active-active / active-passive. */
	nbdev_ch->mp_selector = nbdev->mp_selector;   /* [한국어] round-robin / queue-depth 등. */
	nbdev_ch->rr_min_io = nbdev->rr_min_io;       /* [한국어] RR 전환 전 최소 IO 수. */

	/* [한국어] bdev에 속한 모든 namespace(=path)마다 채널 내 io_path를 만든다. */
	TAILQ_FOREACH(nvme_ns, &nbdev->nvme_ns_list, tailq) {
		rc = _bdev_nvme_add_io_path(nbdev_ch, nvme_ns);
		if (rc != 0) {
			/* [한국어] 하나라도 실패하면 채널을 만들 수 없다 → 락 풀고 롤백 후 에러. */
			pthread_mutex_unlock(&nbdev->mutex);

			_bdev_nvme_delete_io_paths(nbdev_ch);
			return rc;
		}
	}
	pthread_mutex_unlock(&nbdev->mutex);     /* [한국어] 리스트 순회 완료 → 락 해제. */

	return 0;     /* [한국어] 모든 경로 생성 성공 → 채널 사용 가능. */
}

/* If cpl != NULL, complete the bdev_io with nvme status based on 'cpl'.
 * If cpl == NULL, complete the bdev_io with bdev status based on 'status'.
 */
/*
 * [한국어]
 * __bdev_nvme_io_complete - bdev_io를 bdev 코어로 완료 보고하는 최종 헬퍼.
 *
 * @bdev_io: 완료시킬 사용자 IO 요청.
 * @status: cpl이 NULL일 때 사용할 bdev 레벨 상태(SUCCESS/FAILED 등).
 * @cpl: NULL이 아니면 NVMe 컨트롤러가 반환한 CPL — 정확한 SCT/SC 코드를 그대로 전달.
 *
 * NVMe 경로에서 온 IO는 디바이스가 준 16바이트 CPL의 cdw0/sct/sc를 그대로 bdev 코어에
 * 넘겨, 상위 사용자가 NVMe 상태 코드까지 확인할 수 있게 한다(complete_nvme_status).
 * reset/abort처럼 NVMe CPL이 없는 경우엔 cpl=NULL로 bdev 레벨 상태만 보고한다.
 * 완료 직전 trace point(TRACE_BDEV_NVME_IO_DONE)를 찍어 IO 수명 추적을 남긴다.
 * 실행 컨텍스트: IO를 발행한 채널 스레드(완료 콜백은 같은 스레드에서 폴링됨).
 *
 * 호출 체인:
 *   bdev_nvme_*_done 완료 콜백들 → [__bdev_nvme_io_complete]
 *     → spdk_bdev_io_complete[_nvme_status] → 사용자 콜백
 */
static inline void
__bdev_nvme_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status,
			const struct spdk_nvme_cpl *cpl)
{
	/* [한국어] IO 완료 trace point 기록(driver_ctx와 bdev_io 포인터를 인자로). */
	spdk_trace_record(TRACE_BDEV_NVME_IO_DONE, 0, 0, (uintptr_t)bdev_io->driver_ctx,
			  (uintptr_t)bdev_io);
	if (cpl) {
		/* [한국어] NVMe CPL 보유: cdw0 + 상태 코드 타입(sct)/상태 코드(sc)를 그대로 전달. */
		spdk_bdev_io_complete_nvme_status(bdev_io, cpl->cdw0, cpl->status.sct, cpl->status.sc);
	} else {
		/* [한국어] NVMe CPL 없음(reset/abort 등): bdev 레벨 상태로만 완료. */
		spdk_bdev_io_complete(bdev_io, status);
	}
}

/* [한국어] 재시도 큐의 모든 IO를 취소하는 함수의 전방 선언(아래에서 정의). */
static void bdev_nvme_abort_retry_ios(struct nvme_bdev_channel *nbdev_ch);

/*
 * [한국어]
 * bdev_nvme_destroy_bdev_channel_cb - nvme_bdev IO 채널을 파괴하는 콜백.
 *
 * @io_device: nvme_bdev 포인터(미사용 — 시그니처 일치용).
 * @ctx_buf: 파괴할 nvme_bdev_channel 컨텍스트.
 *
 * spdk_put_io_channel의 마지막 참조가 떨어지면 SPDK 프레임워크가 이 콜백을 호출한다.
 * 채널이 사라지기 전에 (1) 재시도 대기 중인 IO들을 모두 취소(완료 보고)하고
 * (2) 채널의 모든 io_path를 떼어 컨트롤러 채널 참조를 반납한다.
 * 실행 컨텍스트: 이 채널을 소유한 스레드.
 *
 * 호출 체인:
 *   spdk_put_io_channel(마지막 참조) → SPDK 코어 → [bdev_nvme_destroy_bdev_channel_cb]
 */
static void
bdev_nvme_destroy_bdev_channel_cb(void *io_device, void *ctx_buf)
{
	struct nvme_bdev_channel *nbdev_ch = ctx_buf;    /* [한국어] 파괴할 채널 컨텍스트. */

	bdev_nvme_abort_retry_ios(nbdev_ch);     /* [한국어] 재시도 대기 IO 전부 취소. */
	_bdev_nvme_delete_io_paths(nbdev_ch);    /* [한국어] 모든 경로 제거 + 채널 참조 반납. */
}

/*
 * [한국어]
 * bdev_nvme_io_type_is_admin - 이 IO 타입이 admin 큐(컨트롤러 단위) 처리를 요하는지 판별.
 *
 * @io_type: bdev IO 타입(READ/WRITE/RESET/NVME_ADMIN/ABORT 등).
 * @return: admin 성격(RESET/NVME_ADMIN/ABORT)이면 true, 일반 IO면 false.
 *
 * 일반 R/W는 per-thread IO qpair로 가지만, reset/admin/abort는 컨트롤러 전체에
 * 영향을 주므로 IO 채널이 아닌 컨트롤러 단위 경로(admin qpair, app 스레드 조율)로
 * 라우팅해야 한다. 이 판별이 그 분기점에서 쓰인다.
 * 실행 컨텍스트: 호출 스레드 무관(순수 함수).
 *
 * 호출 체인:
 *   bdev_nvme_submit_request / io_type_supported 등 → [bdev_nvme_io_type_is_admin]
 */
static inline bool
bdev_nvme_io_type_is_admin(enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_RESET:        /* [한국어] 컨트롤러 reset — admin 성격. */
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:   /* [한국어] raw NVMe admin 명령. */
	case SPDK_BDEV_IO_TYPE_ABORT:        /* [한국어] 진행 중 IO 취소 — admin 성격. */
		return true;
	default:
		break;     /* [한국어] 그 외(R/W/unmap/flush 등)는 일반 IO 경로. */
	}

	return false;
}

/*
 * [한국어]
 * nvme_ns_is_active - namespace가 지금 IO를 발행해도 되는 "활성" 상태인지 판별.
 *
 * @nvme_ns: 검사할 namespace 경로 객체.
 * @return: ANA 갱신 중이 아니고 실제 ns 핸들이 살아 있으면 true.
 *
 * ANA(Asymmetric Namespace Access) 상태를 갱신하는 도중(ana_state_updating)이면
 * 경로 상태가 불확정이라 IO를 보류해야 한다. 또한 namespace가 detach되어 ns 핸들이
 * NULL이면 발행 대상이 없다. 두 경우 모두 비활성으로 본다.
 * 실행 컨텍스트: IO 발행 경로(채널 스레드)에서 hot-path로 호출 → inline.
 *
 * 호출 체인:
 *   nvme_ns_is_accessible / _bdev_nvme_find_io_path_min_qd → [nvme_ns_is_active]
 */
static inline bool
nvme_ns_is_active(struct nvme_ns *nvme_ns)
{
	/* [한국어] ANA 상태 갱신 중이면 경로 상태가 불확정 → 비활성 처리(IO 보류). */
	if (spdk_unlikely(nvme_ns->ana_state_updating)) {
		return false;
	}

	/* [한국어] 실제 NVMe namespace 핸들이 없으면(detach됨) 발행 불가 → 비활성. */
	if (spdk_unlikely(nvme_ns->ns == NULL)) {
		return false;
	}

	return true;     /* [한국어] 갱신 중 아님 + ns 존재 → 활성. */
}

/*
 * [한국어]
 * nvme_ns_is_accessible - namespace가 활성이면서 ANA 상태상 IO를 받을 수 있는지 판별.
 *
 * @nvme_ns: 검사할 namespace 경로.
 * @return: 활성 + ANA가 OPTIMIZED/NON_OPTIMIZED면 true.
 *
 * nvme_ns_is_active(살아 있음) 위에 ANA 접근 가능성을 더한 판정이다. ANA 상태가
 * INACCESSIBLE/PERSISTENT_LOSS/CHANGE 등이면 그 경로로는 IO를 보내면 안 되고,
 * OPTIMIZED(최적 경로) 또는 NON_OPTIMIZED(차선 경로)일 때만 접근 가능으로 본다.
 * 실행 컨텍스트: 경로 선택 hot-path → inline.
 *
 * 호출 체인:
 *   nvme_io_path_is_available → [nvme_ns_is_accessible] → nvme_ns_is_active
 */
static inline bool
nvme_ns_is_accessible(struct nvme_ns *nvme_ns)
{
	/* [한국어] 먼저 살아 있고 갱신 중이 아닌 활성 상태인지 확인. */
	if (spdk_unlikely(!nvme_ns_is_active(nvme_ns))) {
		return false;
	}

	/* [한국어] ANA 접근 상태 판정: 최적/차선 경로만 IO 수용. */
	switch (nvme_ns->ana_state) {
	case SPDK_NVME_ANA_OPTIMIZED_STATE:       /* [한국어] 최적 경로 — 접근 가능. */
	case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:   /* [한국어] 차선 경로 — 접근 가능. */
		return true;
	default:
		break;     /* [한국어] INACCESSIBLE/LOSS/CHANGE 등 → 접근 불가. */
	}

	return false;
}

/*
 * [한국어]
 * nvme_qpair_is_connected - 이 qpair가 지금 IO를 발행할 수 있는 정상 연결 상태인지 판별.
 *
 * @nvme_qpair: 검사할 qpair 래퍼.
 * @return: 실제 qpair 존재 + 실패 사유 없음 + reset 진행 중 아님이면 true.
 *
 * IO를 보내기 전 경로의 전송 채널(qpair)이 건강한지 확인한다. 세 가지를 본다:
 *   1) qpair->qpair(lib/nvme의 실제 qpair) 핸들 존재 — disconnect되면 NULL.
 *   2) 전송 실패 사유 없음 — fabrics link down 등이면 FAILURE_*가 셋됨.
 *   3) 이 컨트롤러 채널이 reset 순회(reset_iter) 중이 아님 — reset 중엔 발행 금지.
 * 실행 컨텍스트: 경로 선택 hot-path → inline.
 *
 * 호출 체인:
 *   nvme_io_path_is_available / _bdev_nvme_find_io_path_min_qd → [nvme_qpair_is_connected]
 *     → spdk_nvme_qpair_get_failure_reason
 */
static inline bool
nvme_qpair_is_connected(struct nvme_qpair *nvme_qpair)
{
	/* [한국어] 실제 lib/nvme qpair 핸들이 없으면(미연결/disconnect) 발행 불가. */
	if (spdk_unlikely(nvme_qpair->qpair == NULL)) {
		return false;
	}

	/* [한국어] 전송 계층이 실패 사유를 보고했으면(예: fabrics link loss) 발행 불가. */
	if (spdk_unlikely(spdk_nvme_qpair_get_failure_reason(nvme_qpair->qpair) !=
			  SPDK_NVME_QPAIR_FAILURE_NONE)) {
		return false;
	}

	/* [한국어] 컨트롤러 채널이 reset 채널 순회 중이면 일시적으로 발행을 막는다. */
	if (spdk_unlikely(nvme_qpair->ctrlr_ch->reset_iter != NULL)) {
		return false;
	}

	return true;     /* [한국어] 세 조건 모두 통과 → 연결 정상. */
}

/*
 * [한국어]
 * nvme_io_path_is_available - 경로(qpair+ns)가 지금 IO를 받을 수 있는지 종합 판정.
 *
 * @io_path: 검사할 경로.
 * @return: qpair 연결 정상 AND namespace 접근 가능이면 true.
 *
 * 경로 가용성 = 전송 채널(qpair) 건강 + 목적지 namespace(ANA) 접근 가능. 둘 다
 * 만족해야 그 경로로 IO를 보낼 수 있다. 경로 선택 루프에서 후보를 거르는 1차 필터.
 * 실행 컨텍스트: 경로 선택 hot-path → inline.
 *
 * 호출 체인:
 *   _bdev_nvme_find_io_path → [nvme_io_path_is_available]
 *     → nvme_qpair_is_connected / nvme_ns_is_accessible
 */
static inline bool
nvme_io_path_is_available(struct nvme_io_path *io_path)
{
	/* [한국어] 전송 qpair가 연결 정상이 아니면 경로 사용 불가. */
	if (spdk_unlikely(!nvme_qpair_is_connected(io_path->qpair))) {
		return false;
	}

	/* [한국어] 목적지 namespace가 ANA상 접근 불가면 경로 사용 불가. */
	if (spdk_unlikely(!nvme_ns_is_accessible(io_path->nvme_ns))) {
		return false;
	}

	return true;     /* [한국어] 둘 다 OK → 경로 가용. */
}

/*
 * [한국어]
 * nvme_ctrlr_is_failed - 컨트롤러가 "복구 불가/실패"로 간주되어야 하는지 판별.
 *
 * @nvme_ctrlr: 검사할 컨트롤러.
 * @return: 실패(더 이상 정상화를 기대할 수 없음)면 true.
 *
 * 경로가 회복 가능성이 있는지(any_io_path_may_become_available) 판단할 때 쓰인다.
 * 판정 우선순위:
 *   - destruct(파괴 중) → 실패.
 *   - fast_io_fail_timedout(빠른 실패 타임아웃 경과) → 실패.
 *   - resetting(reset 중): reconnect_delay_sec가 설정돼 있으면 재연결을 기대 → 미실패,
 *     아니면 reset 실패 시 회복 수단이 없으므로 실패로 본다.
 *   - reconnect_is_delayed(재연결 지연 대기): 곧 재시도하므로 미실패.
 *   - disabled(사용자가 비활성화) → 실패로 취급(IO 받지 않음).
 *   - 그 외엔 lib/nvme의 ctrlr 실패 플래그를 그대로 따른다.
 * 실행 컨텍스트: 경로 회복 가능성 판정 경로(채널 스레드) → inline.
 *
 * 호출 체인:
 *   any_io_path_may_become_available → [nvme_ctrlr_is_failed]
 *     → spdk_nvme_ctrlr_is_failed
 */
static inline bool
nvme_ctrlr_is_failed(struct nvme_ctrlr *nvme_ctrlr)
{
	/* [한국어] 파괴 진행 중이면 회복 불가 → 실패. */
	if (nvme_ctrlr->destruct) {
		return true;
	}

	/* [한국어] fast I/O fail 타임아웃 경과: 빠른 실패 정책상 더는 기다리지 않음 → 실패. */
	if (nvme_ctrlr->fast_io_fail_timedout) {
		return true;
	}

	/* [한국어] reset 진행 중: 재연결 지연이 설정돼 있으면 회복 기대 → 미실패. */
	if (nvme_ctrlr->resetting) {
		if (nvme_ctrlr->opts.reconnect_delay_sec != 0) {
			return false;
		} else {
			/* [한국어] 재연결 정책이 없으면 reset 실패 = 회복 불가 → 실패. */
			return true;
		}
	}

	/* [한국어] 재연결 지연 대기 중: 곧 재시도하므로 아직 실패 아님. */
	if (nvme_ctrlr->reconnect_is_delayed) {
		return false;
	}

	/* [한국어] 사용자가 명시적으로 disable한 컨트롤러는 IO를 받지 않음 → 실패 취급. */
	if (nvme_ctrlr->disabled) {
		return true;
	}

	/* [한국어] 위 어디에도 해당 없으면 lib/nvme의 컨트롤러 실패 상태를 그대로 따른다. */
	if (spdk_nvme_ctrlr_is_failed(nvme_ctrlr->ctrlr)) {
		return true;
	} else {
		return false;
	}
}

/*
 * [한국어]
 * nvme_ctrlr_is_available - 컨트롤러가 지금 당장 정상 사용 가능한 상태인지 판별.
 *
 * @nvme_ctrlr: 검사할 컨트롤러.
 * @return: 파괴/실패/reset/재연결지연/disabled 어디에도 해당 없으면 true.
 *
 * nvme_ctrlr_is_failed가 "회복 가능성"까지 포함한 판정이라면, 이 함수는 "지금
 * 이 순간 정상 동작 중인가"의 더 엄격한 판정이다. reset 중이거나 재연결 대기 중이면
 * 아직 사용 불가로 본다(failed 판정에선 미실패로 분류되더라도). attach 완료/AER
 * 처리 등에서 컨트롤러를 건드려도 되는지 확인할 때 쓰인다.
 * 실행 컨텍스트: 다양한 컨트롤러 op 경로(주로 app/컨트롤러 스레드).
 *
 * 호출 체인:
 *   각종 컨트롤러 op/AER 핸들러 → [nvme_ctrlr_is_available] → spdk_nvme_ctrlr_is_failed
 */
static bool
nvme_ctrlr_is_available(struct nvme_ctrlr *nvme_ctrlr)
{
	/* [한국어] 파괴 진행 중이면 사용 불가. */
	if (nvme_ctrlr->destruct) {
		return false;
	}

	/* [한국어] lib/nvme이 실패로 마킹했으면 사용 불가. */
	if (spdk_nvme_ctrlr_is_failed(nvme_ctrlr->ctrlr)) {
		return false;
	}

	/* [한국어] reset 진행 중이거나 재연결 지연 대기 중이면 아직 사용 불가. */
	if (nvme_ctrlr->resetting || nvme_ctrlr->reconnect_is_delayed) {
		return false;
	}

	/* [한국어] 사용자가 비활성화한 컨트롤러는 사용 불가. */
	if (nvme_ctrlr->disabled) {
		return false;
	}

	return true;     /* [한국어] 모든 배제 조건 통과 → 현재 정상 사용 가능. */
}

/* Simulate circular linked list. */
/*
 * [한국어]
 * nvme_io_path_get_next - 경로 리스트(STAILQ)를 원형으로 순회하기 위해 다음 경로를 반환.
 *
 * @nbdev_ch: 경로 리스트를 가진 채널.
 * @prev_path: 직전 경로(NULL이면 처음부터).
 * @return: prev 다음 경로, 끝이면 리스트 첫 경로(원형 wrap).
 *
 * STAILQ는 단방향 큐라 원형 구조가 아니지만, 라운드로빈/active-passive 경로 탐색에서는
 * 마지막 경로 다음에 다시 첫 경로로 돌아가는 원형 순회가 필요하다. 이 헬퍼가 "다음이
 * 없으면 처음으로" 규칙으로 원형 링크드 리스트를 흉내 낸다.
 * 실행 컨텍스트: 경로 선택 hot-path → inline.
 *
 * 호출 체인:
 *   _bdev_nvme_find_io_path → [nvme_io_path_get_next]
 */
static inline struct nvme_io_path *
nvme_io_path_get_next(struct nvme_bdev_channel *nbdev_ch, struct nvme_io_path *prev_path)
{
	struct nvme_io_path *next_path;    /* [한국어] 반환할 다음 경로 후보. */

	/* [한국어] 직전 경로가 있으면 그 다음 노드를 시도. */
	if (prev_path != NULL) {
		next_path = STAILQ_NEXT(prev_path, stailq);
		if (next_path != NULL) {
			return next_path;     /* [한국어] 다음 노드 존재 → 그대로 반환. */
		}
	}

	/* [한국어] prev가 NULL이거나 끝에 도달 → 리스트 첫 경로로 wrap(원형 흉내). */
	return STAILQ_FIRST(&nbdev_ch->io_path_list);
}

/*
 * [한국어]
 * _bdev_nvme_find_io_path - ANA 상태 기반으로 다음 사용할 경로를 선택한다(RR/active-passive 공용).
 *
 * @nbdev_ch: 경로 후보를 가진 채널.
 * @return: OPTIMIZED 경로(있으면 우선), 없으면 NON_OPTIMIZED 경로, 없으면 NULL.
 *
 * current_io_path 다음부터 원형으로 한 바퀴 돌며 가용 경로를 찾는다. ANA OPTIMIZED를
 * 만나면 즉시 선택·캐시하고, 그게 없으면 처음 만난 NON_OPTIMIZED를 후보로 들고 있다가
 * 한 바퀴 끝나면 그것을 캐시·반환한다. current부터 시작하는 이유는 라운드로빈 분산
 * (active-active)과 직전 경로 우선(active-passive)을 동시에 자연스럽게 구현하기 위함.
 * non_optimized도 캐시하는 이유: 더 나은 경로가 생기면 ANA 이벤트가 와서 캐시를 비운다.
 * 실행 컨텍스트: IO 발행 경로(채널 스레드), lockless.
 *
 * 호출 체인:
 *   bdev_nvme_find_io_path → [_bdev_nvme_find_io_path]
 *     → nvme_io_path_get_next / nvme_io_path_is_available
 */
static struct nvme_io_path *
_bdev_nvme_find_io_path(struct nvme_bdev_channel *nbdev_ch)
{
	struct nvme_io_path *io_path, *start, *non_optimized = NULL;    /* [한국어] 순회 커서/시작점/차선 후보. */

	/* [한국어] 직전 선택 경로 다음부터 시작(라운드로빈/직전 경로 우선 동작의 출발점). */
	start = nvme_io_path_get_next(nbdev_ch, nbdev_ch->current_io_path);

	io_path = start;
	do {
		/* [한국어] 가용한 경로만 후보로 고려(qpair 연결 + ns 접근 가능). */
		if (spdk_likely(nvme_io_path_is_available(io_path))) {
			switch (io_path->nvme_ns->ana_state) {
			case SPDK_NVME_ANA_OPTIMIZED_STATE:
				/* [한국어] 최적 경로 발견 → 즉시 캐시하고 반환(최우선). */
				nbdev_ch->current_io_path = io_path;
				return io_path;
			case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
				/* [한국어] 차선 경로는 처음 만난 것만 후보로 보관(최적이 없을 때 대비). */
				if (non_optimized == NULL) {
					non_optimized = io_path;
				}
				break;
			default:
				/* [한국어] is_accessible이 걸렀어야 할 상태 — 도달 불가 가정. */
				assert(false);
				break;
			}
		}
		/* [한국어] 다음 경로로 진행(원형). */
		io_path = nvme_io_path_get_next(nbdev_ch, io_path);
	} while (io_path != start);     /* [한국어] 시작점으로 돌아오면 한 바퀴 완료. */

	/* We come here only if there is no optimized path. Cache even non_optimized
	 * path. If any path becomes optimized, ANA event will be received and
	 * cache will be cleared.
	 */
	/* [한국어] (위 영어 주석) 최적 경로가 없을 때만 도달. 차선 경로도 캐시한다 —
	 * 어떤 경로가 최적이 되면 ANA 이벤트가 와서 캐시를 비워 재선택을 유도. */
	nbdev_ch->current_io_path = non_optimized;

	return non_optimized;     /* [한국어] 차선 경로(또는 NULL) 반환. */
}

/*
 * [한국어]
 * _bdev_nvme_find_io_path_min_qd - 큐 깊이가 가장 얕은 경로를 선택한다(queue-depth selector).
 *
 * @nbdev_ch: 경로 후보를 가진 채널.
 * @return: 최소 outstanding 요청을 가진 OPTIMIZED 경로 우선, 없으면 NON_OPTIMIZED, 없으면 NULL.
 *
 * BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH 정책 구현. 모든 가용 경로를 훑어 각 qpair의
 * outstanding 요청 수를 비교하고, ANA 등급별(최적/차선)로 가장 한가한 경로를 고른다.
 * 부하가 적은 경로로 IO를 보내 latency를 균등화한다. RR과 달리 매번 전체를 비교하므로
 * 결과를 캐시하지 않는다(큐 깊이는 매 순간 바뀌므로 캐시가 무의미).
 * 실행 컨텍스트: IO 발행 경로(채널 스레드), lockless.
 *
 * 호출 체인:
 *   bdev_nvme_find_io_path → [_bdev_nvme_find_io_path_min_qd]
 *     → spdk_nvme_qpair_get_num_outstanding_reqs
 */
static struct nvme_io_path *
_bdev_nvme_find_io_path_min_qd(struct nvme_bdev_channel *nbdev_ch)
{
	struct nvme_io_path *io_path;                                    /* [한국어] 순회 커서. */
	struct nvme_io_path *optimized = NULL, *non_optimized = NULL;    /* [한국어] 등급별 최소 큐 경로. */
	uint32_t opt_min_qd = UINT32_MAX, non_opt_min_qd = UINT32_MAX;   /* [한국어] 등급별 현재 최소 큐 깊이. */
	uint32_t num_outstanding_reqs;                                  /* [한국어] 현재 경로 qpair의 미완료 요청 수. */

	/* [한국어] 모든 경로를 선형 순회하며 가장 한가한 경로를 찾는다. */
	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		if (spdk_unlikely(!nvme_qpair_is_connected(io_path->qpair))) {
			/* The device is currently resetting. */
			/* [한국어] qpair 미연결(reset 중 등) → 후보에서 제외. */
			continue;
		}

		/* [한국어] namespace가 비활성이면 후보 제외(ANA 등급은 아래에서 별도 판정). */
		if (spdk_unlikely(!nvme_ns_is_active(io_path->nvme_ns))) {
			continue;
		}

		/* [한국어] 이 경로 qpair에 현재 발행되어 완료 대기 중인 요청 수 조회(큐 깊이). */
		num_outstanding_reqs = spdk_nvme_qpair_get_num_outstanding_reqs(io_path->qpair->qpair);
		switch (io_path->nvme_ns->ana_state) {
		case SPDK_NVME_ANA_OPTIMIZED_STATE:
			/* [한국어] 최적 등급 내에서 더 한가한 경로면 갱신. */
			if (num_outstanding_reqs < opt_min_qd) {
				opt_min_qd = num_outstanding_reqs;
				optimized = io_path;
			}
			break;
		case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
			/* [한국어] 차선 등급 내에서 더 한가한 경로면 갱신(최적이 없을 때 대비). */
			if (num_outstanding_reqs < non_opt_min_qd) {
				non_opt_min_qd = num_outstanding_reqs;
				non_optimized = io_path;
			}
			break;
		default:
			break;     /* [한국어] 접근 불가 ANA 상태는 무시. */
		}
	}

	/* don't cache io path for BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH selector */
	/* [한국어] (위 영어 주석) 큐 깊이는 매 순간 변하므로 결과를 current_io_path에 캐시하지 않는다. */
	if (optimized != NULL) {
		return optimized;     /* [한국어] 최적 등급의 최소 큐 경로 우선 반환. */
	}

	return non_optimized;     /* [한국어] 최적이 없으면 차선의 최소 큐 경로(또는 NULL). */
}

/*
 * [한국어]
 * bdev_nvme_find_io_path - 멀티패스 정책에 따라 이번 IO에 쓸 경로를 결정하는 진입점.
 *
 * @nbdev_ch: 경로 선택 대상 채널.
 * @return: 선택된 io_path, 가용 경로가 없으면 NULL.
 *
 * 경로 선택의 fast-path 최적화 + 정책 디스패치를 함께 한다:
 *   1) 캐시된 current_io_path가 있으면 —
 *      - active-passive: 그대로 재사용(같은 경로 고수).
 *      - round-robin: rr_counter를 올려 rr_min_io 도달 전엔 캐시 경로 재사용,
 *        도달하면 카운터를 리셋하고 아래에서 다음 경로를 새로 고른다.
 *   2) 캐시가 없거나 RR 전환 시점이면 정책별 탐색 함수로 위임:
 *      - active-passive 또는 RR → _bdev_nvme_find_io_path(ANA 우선 순회).
 *      - queue-depth → _bdev_nvme_find_io_path_min_qd(최소 큐).
 * RR에서 rr_min_io만큼 같은 경로를 연속 사용하는 이유: 경로 전환 비용/캐시 효율 때문.
 * 실행 컨텍스트: IO 발행 hot-path(채널 스레드) → inline, lockless.
 *
 * 호출 체인:
 *   bdev_nvme_submit_request → [bdev_nvme_find_io_path]
 *     → _bdev_nvme_find_io_path / _bdev_nvme_find_io_path_min_qd
 */
static inline struct nvme_io_path *
bdev_nvme_find_io_path(struct nvme_bdev_channel *nbdev_ch)
{
	/* [한국어] 캐시된 경로가 있으면 정책에 따라 재사용 여부 판정(전체 탐색 회피). */
	if (spdk_likely(nbdev_ch->current_io_path != NULL)) {
		if (nbdev_ch->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE) {
			/* [한국어] active-passive: 현재 경로를 계속 고수. */
			return nbdev_ch->current_io_path;
		} else if (nbdev_ch->mp_selector == BDEV_NVME_MP_SELECTOR_ROUND_ROBIN) {
			/* [한국어] RR: rr_min_io 횟수까지는 같은 경로 유지, 그 전까진 캐시 재사용. */
			if (++nbdev_ch->rr_counter < nbdev_ch->rr_min_io) {
				return nbdev_ch->current_io_path;
			}
			/* [한국어] 임계 도달 → 카운터 리셋하고 아래에서 다음 경로 재선택. */
			nbdev_ch->rr_counter = 0;
		}
	}

	/* [한국어] 정책별 탐색 함수로 위임: ANA 순회(active-passive/RR) vs 최소 큐(queue-depth). */
	if (nbdev_ch->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE ||
	    nbdev_ch->mp_selector == BDEV_NVME_MP_SELECTOR_ROUND_ROBIN) {
		return _bdev_nvme_find_io_path(nbdev_ch);
	} else {
		return _bdev_nvme_find_io_path_min_qd(nbdev_ch);
	}
}

/* Return true if there is any io_path whose qpair is active or ctrlr is not failed,
 * or false otherwise.
 *
 * If any io_path has an active qpair but find_io_path() returned NULL, its namespace
 * is likely to be non-accessible now but may become accessible.
 *
 * If any io_path has an unfailed ctrlr but find_io_path() returned NULL, the ctrlr
 * is likely to be resetting now but the reset may succeed. A ctrlr is set to unfailed
 * when starting to reset it but it is set to failed when the reset failed. Hence, if
 * a ctrlr is unfailed, it is likely that it works fine or is resetting.
 */
/*
 * [한국어]
 * any_io_path_may_become_available - 지금은 못 쓰지만 곧 가용해질 경로가 있는지 판정.
 *
 * @nbdev_ch: 검사할 채널.
 * @return: 회복 가능성 있는 경로가 하나라도 있으면 true, 전부 가망 없으면 false.
 *
 * find_io_path()가 NULL을 반환했을 때 IO를 즉시 실패시킬지, 아니면 재시도 큐에 넣고
 * 기다릴지를 결정하는 핵심 판정이다. 위 영어 주석의 논리:
 *   - qpair가 연결되어 있으면(하지만 ns가 INACCESSIBLE이라 선택 안 됨) → ANA가 바뀌어
 *     접근 가능해질 수 있으므로 회복 가능.
 *   - ctrlr가 failed가 아니면(=정상이거나 reset 중) → reset이 성공할 수 있으므로 회복 가능.
 * 채널이 reset 중(nbdev_ch->resetting)이거나 모든 경로의 ANA 전이가 타임아웃됐으면
 * 가망 없음으로 본다.
 * 실행 컨텍스트: IO 발행 실패 후 재시도 결정 경로(채널 스레드).
 *
 * 호출 체인:
 *   bdev_nvme_submit_request / _bdev_nvme_submit_request → [any_io_path_may_become_available]
 *     → nvme_qpair_is_connected / nvme_ctrlr_is_failed
 */
static bool
any_io_path_may_become_available(struct nvme_bdev_channel *nbdev_ch)
{
	struct nvme_io_path *io_path;    /* [한국어] 경로 순회 커서. */

	/* [한국어] 채널 자체가 reset 중이면 경로들이 정리되는 중 → 회복 기대 안 함. */
	if (nbdev_ch->resetting) {
		return false;
	}

	/* [한국어] 각 경로에 대해 회복 가능성을 검사. */
	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		/* [한국어] ANA 전이가 이미 타임아웃된 경로는 회복 가망 없음 → 건너뜀. */
		if (io_path->nvme_ns->ana_transition_timedout) {
			continue;
		}

		/* [한국어] qpair 연결됨(ANA 변화 기대) 또는 ctrlr 미실패(reset 성공 기대) → 회복 가능. */
		if (nvme_qpair_is_connected(io_path->qpair) ||
		    !nvme_ctrlr_is_failed(io_path->qpair->ctrlr)) {
			return true;
		}
	}

	return false;     /* [한국어] 모든 경로가 가망 없음 → IO를 실패시켜야 함. */
}

/*
 * [한국어]
 * bdev_nvme_retry_io - 재시도 큐에서 꺼낸 IO를 적절한 경로로 다시 발행한다.
 *
 * @nbdev_ch: IO가 속한 채널.
 * @bdev_io: 재발행할 사용자 IO.
 *
 * 이전에 시도했던 경로(nbdev_io->io_path)가 아직 가용하면 그 경로로 빠르게 재제출
 * (_bdev_nvme_submit_request, 경로 재선택 생략). 그 경로가 사라졌거나 불가하면
 * 전체 제출 경로(bdev_nvme_submit_request)로 들어가 경로를 처음부터 다시 고른다.
 * 실행 컨텍스트: 재시도 poller(채널 스레드)에서 호출.
 *
 * 호출 체인:
 *   bdev_nvme_retry_ios(poller) → [bdev_nvme_retry_io]
 *     → _bdev_nvme_submit_request / bdev_nvme_submit_request
 */
static void
bdev_nvme_retry_io(struct nvme_bdev_channel *nbdev_ch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_bdev_io *nbdev_io = (struct nvme_bdev_io *)bdev_io->driver_ctx;    /* [한국어] bdev_io에 묶인 모듈 컨텍스트. */
	struct spdk_io_channel *ch;    /* [한국어] 전체 제출 경로로 갈 때 필요한 채널 핸들. */

	/* [한국어] 직전 경로가 살아 있으면 경로 재선택 없이 바로 그 경로로 재제출(빠른 경로). */
	if (nbdev_io->io_path != NULL && nvme_io_path_is_available(nbdev_io->io_path)) {
		_bdev_nvme_submit_request(nbdev_ch, bdev_io);
	} else {
		/* [한국어] 직전 경로 불가 → 채널 핸들 복원 후 전체 제출 경로로 재선택. */
		ch = spdk_io_channel_from_ctx(nbdev_ch);
		bdev_nvme_submit_request(ch, bdev_io);
	}
}

/*
 * [한국어]
 * bdev_nvme_retry_ios - 만료된 재시도 IO들을 재발행하고 다음 재시도 타이머를 다시 건다.
 *
 * @arg: nvme_bdev_channel 포인터(poller 등록 시 넘긴 컨텍스트).
 * @return: 항상 SPDK_POLLER_BUSY(매 호출마다 작업 수행으로 간주).
 *
 * 재시도 큐(retry_io_list)는 retry_ticks 오름차순으로 정렬되어 있다. 현재 tsc(now)를
 * 지난 IO들을 앞에서부터 꺼내 재발행하고, 아직 시각이 안 된 IO를 만나면 중단한다.
 * 그 후 기존 타이머 poller를 해제하고, 큐에 남은 가장 이른 IO가 있으면 그 시각까지의
 * 지연(delay_us)으로 타이머 poller를 다시 등록한다(self-rearming oneshot 패턴).
 * 실행 컨텍스트: 채널 스레드의 poller. 큐는 같은 스레드에서만 접근 → lockless.
 *
 * 호출 체인:
 *   SPDK reactor poller 루프 → [bdev_nvme_retry_ios] → bdev_nvme_retry_io
 */
static int
bdev_nvme_retry_ios(void *arg)
{
	struct nvme_bdev_channel *nbdev_ch = arg;    /* [한국어] 재시도 큐를 가진 채널. */
	struct nvme_bdev_io *bio, *tmp_bio;          /* [한국어] 순회 커서 + 제거 안전용 다음 노드. */
	uint64_t now, delay_us;                      /* [한국어] 현재 tsc, 다음 타이머 지연(us). */

	now = spdk_get_ticks();    /* [한국어] 현재 TSC 틱 — retry_ticks와 비교 기준. */

	/* [한국어] 재시도 시각이 도래한 IO를 앞에서부터 꺼내 재발행. */
	TAILQ_FOREACH_SAFE(bio, &nbdev_ch->retry_io_list, retry_link, tmp_bio) {
		/* [한국어] 정렬되어 있으므로, 아직 시각 안 된 IO를 만나면 이후는 모두 미래 → 중단. */
		if (bio->retry_ticks > now) {
			break;
		}

		/* [한국어] 큐에서 제거 후 재발행. */
		TAILQ_REMOVE(&nbdev_ch->retry_io_list, bio, retry_link);

		bdev_nvme_retry_io(nbdev_ch, spdk_bdev_io_from_ctx(bio));
	}

	/* [한국어] 현재 타이머 poller 해제(oneshot이므로 매번 재등록). */
	spdk_poller_unregister(&nbdev_ch->retry_io_poller);

	/* [한국어] 큐에 남은 가장 이른 IO가 있으면 그 시각까지 지연 타이머를 다시 건다. */
	bio = TAILQ_FIRST(&nbdev_ch->retry_io_list);
	if (bio != NULL) {
		/* [한국어] (retry_ticks - now) 틱을 마이크로초로 환산: ×1e6 / ticks_hz. */
		delay_us = (bio->retry_ticks - now) * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();

		/* [한국어] delay_us 후 한 번 깨어나는 타이머 poller 재등록. */
		nbdev_ch->retry_io_poller = SPDK_POLLER_REGISTER(bdev_nvme_retry_ios, nbdev_ch,
					    delay_us);
	}

	return SPDK_POLLER_BUSY;    /* [한국어] poller에 "일을 했다"고 보고. */
}

/*
 * [한국어]
 * bdev_nvme_queue_retry_io - 일시 실패한 IO를 delay_ms 후 재시도하도록 정렬 큐에 삽입한다.
 *
 * @nbdev_ch: 재시도 큐를 가진 채널.
 * @bio: 재시도 대기시킬 IO.
 * @delay_ms: 지금부터 몇 ms 뒤에 재시도할지.
 *
 * retry_ticks를 (현재 + delay)로 계산하고, 큐를 retry_ticks 오름차순으로 유지하기 위해
 * 뒤에서부터 역방향 순회하며 자기보다 이른(또는 같은) IO 뒤에 삽입한다. 더 이른 IO가
 * 전혀 없으면 새 head가 되며, 이 경우 타이머 poller를 delay_ms로 다시 건다(가장 이른
 * IO가 바뀌었으므로). head가 아니면 기존 타이머가 더 이르므로 재설정하지 않는다.
 * 실행 컨텍스트: IO 발행 중 일시 실패를 감지한 채널 스레드. lockless.
 *
 * 호출 체인:
 *   bdev_nvme_io_complete_nvme_status / submit 실패 경로 → [bdev_nvme_queue_retry_io]
 */
static void
bdev_nvme_queue_retry_io(struct nvme_bdev_channel *nbdev_ch,
			 struct nvme_bdev_io *bio, uint64_t delay_ms)
{
	struct nvme_bdev_io *tmp_bio;    /* [한국어] 삽입 위치를 찾기 위한 역방향 순회 커서. */

	/* [한국어] 재시도 절대 시각 = 현재 TSC + delay_ms를 틱으로 환산. */
	bio->retry_ticks = spdk_get_ticks() + delay_ms * spdk_get_ticks_hz() / 1000ULL;

	/* [한국어] 큐를 뒤에서부터 훑어 자기보다 이르거나 같은 IO 바로 뒤에 끼워 정렬 유지. */
	TAILQ_FOREACH_REVERSE(tmp_bio, &nbdev_ch->retry_io_list, retry_io_head, retry_link) {
		if (tmp_bio->retry_ticks <= bio->retry_ticks) {
			TAILQ_INSERT_AFTER(&nbdev_ch->retry_io_list, tmp_bio, bio,
					   retry_link);
			return;     /* [한국어] head가 아니므로 타이머 재설정 불필요. */
		}
	}

	/* No earlier I/Os were found. This I/O must be the new head. */
	/* [한국어] (위 영어 주석) 더 이른 IO가 없으면 이 IO가 새 head → 타이머도 갱신해야 함. */
	TAILQ_INSERT_HEAD(&nbdev_ch->retry_io_list, bio, retry_link);

	/* [한국어] 기존 타이머 해제 후, 더 이른 새 head 시각(delay_ms)으로 타이머 재등록. */
	spdk_poller_unregister(&nbdev_ch->retry_io_poller);

	nbdev_ch->retry_io_poller = SPDK_POLLER_REGISTER(bdev_nvme_retry_ios, nbdev_ch,
				    delay_ms * 1000ULL);
}

/*
 * [한국어]
 * bdev_nvme_abort_retry_ios - 재시도 큐의 모든 IO를 ABORTED로 완료시킨다(채널 파괴 등).
 *
 * @nbdev_ch: 재시도 큐를 비울 채널.
 *
 * 채널이 파괴되거나 더 이상 재시도가 무의미할 때, 대기 중인 모든 IO를 큐에서 빼고
 * SPDK_BDEV_IO_STATUS_ABORTED로 사용자에게 완료 보고한다(영구 누락 방지). 마지막에
 * 타이머 poller도 해제한다.
 * 실행 컨텍스트: 채널 스레드(파괴 콜백 등).
 *
 * 호출 체인:
 *   bdev_nvme_destroy_bdev_channel_cb 등 → [bdev_nvme_abort_retry_ios]
 *     → __bdev_nvme_io_complete
 */
static void
bdev_nvme_abort_retry_ios(struct nvme_bdev_channel *nbdev_ch)
{
	struct nvme_bdev_io *bio, *tmp_bio;    /* [한국어] 순회 커서 + 제거 안전용 다음 노드. */

	/* [한국어] 큐의 모든 IO를 제거하며 ABORTED로 완료(cpl=NULL → bdev 레벨 상태). */
	TAILQ_FOREACH_SAFE(bio, &nbdev_ch->retry_io_list, retry_link, tmp_bio) {
		TAILQ_REMOVE(&nbdev_ch->retry_io_list, bio, retry_link);
		__bdev_nvme_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_ABORTED, NULL);
	}

	spdk_poller_unregister(&nbdev_ch->retry_io_poller);    /* [한국어] 더 처리할 게 없으니 타이머 해제. */
}

/*
 * [한국어]
 * bdev_nvme_abort_retry_io - 재시도 큐에서 특정 한 IO만 찾아 ABORTED로 완료시킨다.
 *
 * @nbdev_ch: 재시도 큐를 가진 채널.
 * @bio_to_abort: 취소 대상 IO.
 * @return: 큐에서 찾아 취소했으면 0, 큐에 없으면 -ENOENT.
 *
 * 사용자가 ABORT 명령으로 특정 IO를 취소할 때, 그 IO가 아직 재시도 대기 중이면
 * 여기서 처리한다. 큐에 없으면(-ENOENT) 이미 디바이스에 발행된 상태이므로 호출자는
 * NVMe Abort 명령 경로로 넘어간다.
 * 실행 컨텍스트: 채널 스레드.
 *
 * 호출 체인:
 *   bdev_nvme_abort(ABORT IO 처리) → [bdev_nvme_abort_retry_io]
 *     → __bdev_nvme_io_complete
 */
static int
bdev_nvme_abort_retry_io(struct nvme_bdev_channel *nbdev_ch,
			 struct nvme_bdev_io *bio_to_abort)
{
	struct nvme_bdev_io *bio;    /* [한국어] 큐 순회 커서. */

	/* [한국어] 큐를 훑어 대상 IO를 찾는다. */
	TAILQ_FOREACH(bio, &nbdev_ch->retry_io_list, retry_link) {
		if (bio == bio_to_abort) {
			/* [한국어] 발견: 큐에서 제거하고 ABORTED로 완료 후 성공 반환. */
			TAILQ_REMOVE(&nbdev_ch->retry_io_list, bio, retry_link);
			__bdev_nvme_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_ABORTED, NULL);
			return 0;
		}
	}

	return -ENOENT;    /* [한국어] 큐에 없음 → 이미 디바이스 발행됨(상위가 NVMe Abort 시도). */
}

/*
 * [한국어]
 * bdev_nvme_update_nvme_error_stat - 에러 CPL을 받았을 때 NVMe 상태 코드별 통계를 누적한다.
 *
 * @bdev_io: 에러로 완료된 IO(소속 nbdev로 통계 위치 추적).
 * @cpl: 디바이스가 반환한 에러 CPL(sct/sc 포함).
 *
 * 진단/모니터링용으로, 에러의 상태 코드 타입(SCT)별 카운트와 (SCT, SC) 2차원
 * 카운트를 누적한다. SCT는 GENERIC/COMMAND_SPECIFIC/MEDIA_ERROR/PATH 4종에 한해
 * 2차원 카운트를 기록(그 외 벤더 특정 등은 타입 카운트만). err_stat 배열은 여러
 * 채널 스레드가 동시에 갱신할 수 있어 nbdev->mutex로 보호한다.
 * 실행 컨텍스트: IO 완료 콜백(채널 스레드). 통계 배열 공유 → mutex 필요.
 *
 * 호출 체인:
 *   bdev_nvme_io_complete_nvme_status(에러 경로) → [bdev_nvme_update_nvme_error_stat]
 */
static void
bdev_nvme_update_nvme_error_stat(struct spdk_bdev_io *bdev_io, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev *nbdev;    /* [한국어] 통계를 보관하는 논리 bdev. */
	uint16_t sct, sc;           /* [한국어] 상태 코드 타입과 상태 코드. */

	/* [한국어] 이 함수는 에러 CPL에만 호출되어야 함 — 불변식 검증. */
	assert(spdk_nvme_cpl_is_error(cpl));

	/* [한국어] bdev 컨텍스트에서 nvme_bdev 복원(통계 배열 소유자). */
	nbdev = bdev_io->bdev->ctxt;

	/* [한국어] 에러 통계 수집이 비활성(배열 미할당)이면 아무것도 안 함. */
	if (nbdev->err_stat == NULL) {
		return;
	}

	sct = cpl->status.sct;    /* [한국어] Status Code Type (NVMe spec §4.6.1.2.1). */
	sc = cpl->status.sc;      /* [한국어] Status Code. */

	/* [한국어] 통계 배열은 다중 채널에서 동시 갱신 가능 → 락 보호. */
	pthread_mutex_lock(&nbdev->mutex);

	/* [한국어] SCT별 발생 횟수 누적(타입 단위 집계). */
	nbdev->err_stat->status_type[sct]++;
	switch (sct) {
	/* [한국어] 표준 SCT 4종에 한해 (SCT, SC) 2차원 세부 카운트 기록. */
	case SPDK_NVME_SCT_GENERIC:
	case SPDK_NVME_SCT_COMMAND_SPECIFIC:
	case SPDK_NVME_SCT_MEDIA_ERROR:
	case SPDK_NVME_SCT_PATH:
		nbdev->err_stat->status[sct][sc]++;
		break;
	default:
		break;     /* [한국어] 벤더 특정 등은 타입 카운트만 유지. */
	}

	pthread_mutex_unlock(&nbdev->mutex);    /* [한국어] 통계 갱신 완료 → 락 해제. */
}

/*
 * [한국어]
 * bdev_nvme_update_io_path_stat - 완료된 IO의 바이트 수/연산 수/지연을 경로별 통계에 누적한다.
 *
 * @bio: 막 완료된 IO의 모듈 컨텍스트.
 *
 * g_opts.io_path_stat가 켜져 경로별 stat 버퍼가 있을 때만 동작한다. submit_tsc부터
 * 지금까지의 틱 차(tsc_diff)를 지연으로 계산하고, IO 타입(READ/WRITE/UNMAP/ZCOPY/COPY)
 * 별로 바이트/op 카운트와 누적/최대/최소 지연을 갱신한다. ZCOPY는 start 단계에서만,
 * populate면 read로 아니면 write로 집계한다. stat은 경로 소유 스레드에서만 갱신되어
 * lockless다(경로 삭제 후에도 in-flight IO가 갱신할 수 있어 지연 free 정책과 짝).
 * 실행 컨텍스트: IO 완료 콜백(경로 소유 채널 스레드) → inline.
 *
 * 호출 체인:
 *   bdev_nvme_io_complete_nvme_status(성공 경로) → [bdev_nvme_update_io_path_stat]
 */
static inline void
bdev_nvme_update_io_path_stat(struct nvme_bdev_io *bio)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);    /* [한국어] 완료된 사용자 IO. */
	uint64_t num_blocks = bdev_io->u.bdev.num_blocks;            /* [한국어] 전송 블록 수. */
	uint32_t blocklen = bdev_io->bdev->blocklen;                /* [한국어] 블록당 바이트(바이트 환산용). */
	struct spdk_bdev_io_stat *stat;                             /* [한국어] 갱신 대상 경로 통계. */
	uint64_t tsc_diff;                                          /* [한국어] 발행~완료 지연(틱). */

	/* [한국어] 경로별 통계가 비활성이면 아무것도 안 함. */
	if (bio->io_path->stat == NULL) {
		return;
	}

	tsc_diff = spdk_get_ticks() - bio->submit_tsc;    /* [한국어] 지연 = 현재 틱 - 발행 시점 틱. */
	stat = bio->io_path->stat;

	/* [한국어] IO 타입별로 바이트/연산수/지연(누적·최대·최소)을 누적. */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		stat->bytes_read += num_blocks * blocklen;     /* [한국어] 읽은 바이트 누적. */
		stat->num_read_ops++;                          /* [한국어] 읽기 연산 수 +1. */
		stat->read_latency_ticks += tsc_diff;          /* [한국어] 읽기 누적 지연. */
		/* [한국어] 최대/최소 read 지연 갱신(꼬리 지연 추적). */
		if (stat->max_read_latency_ticks < tsc_diff) {
			stat->max_read_latency_ticks = tsc_diff;
		}
		if (stat->min_read_latency_ticks > tsc_diff) {
			stat->min_read_latency_ticks = tsc_diff;
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* [한국어] 쓰기 바이트/연산수/지연(누적·최대·최소) 갱신. */
		stat->bytes_written += num_blocks * blocklen;
		stat->num_write_ops++;
		stat->write_latency_ticks += tsc_diff;
		if (stat->max_write_latency_ticks < tsc_diff) {
			stat->max_write_latency_ticks = tsc_diff;
		}
		if (stat->min_write_latency_ticks > tsc_diff) {
			stat->min_write_latency_ticks = tsc_diff;
		}
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		/* [한국어] UNMAP(deallocate) 바이트/연산수/지연 갱신. */
		stat->bytes_unmapped += num_blocks * blocklen;
		stat->num_unmap_ops++;
		stat->unmap_latency_ticks += tsc_diff;
		if (stat->max_unmap_latency_ticks < tsc_diff) {
			stat->max_unmap_latency_ticks = tsc_diff;
		}
		if (stat->min_unmap_latency_ticks > tsc_diff) {
			stat->min_unmap_latency_ticks = tsc_diff;
		}
		break;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		/* Track the data in the start phase only */
		/* [한국어] zero-copy는 start 단계에서만 데이터량을 집계(end는 메타 처리). */
		if (!bdev_io->u.bdev.zcopy.start) {
			break;
		}
		/* [한국어] populate(디바이스→호스트 채움)면 read, 아니면 write로 집계. */
		if (bdev_io->u.bdev.zcopy.populate) {
			/* [한국어] populate = 디바이스에서 데이터를 읽어 채움 → read 통계. */
			stat->bytes_read += num_blocks * blocklen;
			stat->num_read_ops++;
			stat->read_latency_ticks += tsc_diff;
			if (stat->max_read_latency_ticks < tsc_diff) {
				stat->max_read_latency_ticks = tsc_diff;
			}
			if (stat->min_read_latency_ticks > tsc_diff) {
				stat->min_read_latency_ticks = tsc_diff;
			}
		} else {
			/* [한국어] populate 아님 = 호스트 데이터를 디바이스에 씀 → write 통계. */
			stat->bytes_written += num_blocks * blocklen;
			stat->num_write_ops++;
			stat->write_latency_ticks += tsc_diff;
			if (stat->max_write_latency_ticks < tsc_diff) {
				stat->max_write_latency_ticks = tsc_diff;
			}
			if (stat->min_write_latency_ticks > tsc_diff) {
				stat->min_write_latency_ticks = tsc_diff;
			}
		}
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		/* [한국어] COPY(SCC, simple copy) 바이트/연산수/지연 갱신. */
		stat->bytes_copied += num_blocks * blocklen;
		stat->num_copy_ops++;
		stat->copy_latency_ticks += tsc_diff;
		if (stat->max_copy_latency_ticks < tsc_diff) {
			stat->max_copy_latency_ticks = tsc_diff;
		}
		if (stat->min_copy_latency_ticks > tsc_diff) {
			stat->min_copy_latency_ticks = tsc_diff;
		}
		break;
	default:
		break;     /* [한국어] flush/reset 등 데이터 없는 IO는 통계 미집계. */
	}
}

/*
 * [한국어]
 * bdev_nvme_check_retry_io - 에러로 끝난 IO를 재시도해야 하는지, 한다면 지연을 얼마로 할지 결정.
 *
 * @bio: 에러로 완료된 IO.
 * @cpl: 에러 CPL.
 * @nbdev_ch: IO가 속한 채널.
 * @_delay_ms: [out] 재시도까지 대기할 ms.
 * @return: 재시도 가능하면 true(+ *_delay_ms 설정), 재시도 무의미하면 false(즉시 실패).
 *
 * 두 갈래로 나뉜다:
 *   (A) 경로/컨트롤러 문제(path error, SQ deletion, 경로/컨트롤러 비가용): 현재 경로를
 *       버리고(clear current + bio->io_path=NULL) 다른 경로로 옮기려 시도한다. ANA 에러면
 *       ANA log를 다시 읽어 상태를 갱신 표시한다. 회복 가능 경로가 하나도 없으면 false.
 *       있으면 지연 0(즉시 다른 경로로 재시도).
 *   (B) 그 외 일시적 에러: 같은 경로로 재시도하되 retry_count를 올리고, 컨트롤러가 알려준
 *       Command Retry Delay(CRD index → crdt[idx]×100ms)를 지연으로 사용한다(없으면 0).
 * 실행 컨텍스트: IO 완료 콜백(채널 스레드).
 *
 * 호출 체인:
 *   bdev_nvme_io_complete_nvme_status → [bdev_nvme_check_retry_io]
 *     → nvme_ctrlr_read_ana_log_page / any_io_path_may_become_available
 */
static bool
bdev_nvme_check_retry_io(struct nvme_bdev_io *bio,
			 const struct spdk_nvme_cpl *cpl,
			 struct nvme_bdev_channel *nbdev_ch,
			 uint64_t *_delay_ms)
{
	struct nvme_io_path *io_path = bio->io_path;                    /* [한국어] 실패한 IO가 쓰던 경로. */
	struct nvme_ctrlr *nvme_ctrlr = io_path->qpair->ctrlr;          /* [한국어] 그 경로의 컨트롤러. */
	const struct spdk_nvme_ctrlr_data *cdata;                       /* [한국어] CRD 조회용 컨트롤러 데이터. */

	/* [한국어] (A) 경로/컨트롤러 차원의 실패: 다른 경로로 옮겨야 하는 상황. */
	if (spdk_nvme_cpl_is_path_error(cpl) ||
	    spdk_nvme_cpl_is_aborted_sq_deletion(cpl) ||
	    !nvme_io_path_is_available(io_path) ||
	    !nvme_ctrlr_is_available(nvme_ctrlr)) {
		/* [한국어] 캐시된 현재 경로와 이 IO의 경로 기억을 모두 비워 재선택 유도. */
		bdev_nvme_clear_current_io_path(nbdev_ch);
		bio->io_path = NULL;
		/* [한국어] ANA 에러면 최신 ANA log를 다시 읽고, 성공 시 갱신 중 표시. */
		if (spdk_nvme_cpl_is_ana_error(cpl)) {
			if (nvme_ctrlr_read_ana_log_page(nvme_ctrlr) == 0) {
				io_path->nvme_ns->ana_state_updating = true;
			}
		}
		/* [한국어] 회복 가능 경로가 전무하면 재시도 의미 없음 → false(즉시 실패). */
		if (!any_io_path_may_become_available(nbdev_ch)) {
			return false;
		}
		*_delay_ms = 0;    /* [한국어] 다른 경로가 있으니 지연 없이 즉시 재시도. */
	} else {
		/* [한국어] (B) 일시적 에러: 같은 경로 재시도, 횟수 증가. */
		bio->retry_count++;

		/* [한국어] 컨트롤러가 권고한 재시도 지연(CRD)을 조회. */
		cdata = spdk_nvme_ctrlr_get_data(nvme_ctrlr->ctrlr);

		/* [한국어] CRD index가 있으면 crdt[index]×100ms를 지연으로, 없으면 0. */
		if (cpl->status.crd != 0) {
			*_delay_ms = cdata->crdt[cpl->status.crd] * 100;
		} else {
			*_delay_ms = 0;
		}
	}

	return true;    /* [한국어] 재시도 진행. */
}

/*
 * [한국어]
 * bdev_nvme_io_complete_nvme_status - NVMe IO 완료 CPL을 받아 성공/재시도/실패를 결정하고 처리.
 *
 * @bio: 완료된 IO의 모듈 컨텍스트.
 * @cpl: 디바이스가 반환한 CPL.
 *
 * 일반 R/W류 IO(admin 제외)의 완료 진입점. 흐름:
 *   1) 성공이면 경로 통계 갱신 후 즉시 완료(complete).
 *   2) 실패면 에러 통계를 먼저 누적(재시도와 무관하게 카운트).
 *   3) DNR(Do Not Retry) 비트, 사용자 abort, 재시도 횟수 초과면 더 시도 않고 완료.
 *   4) accel_sequence가 걸려 있으면 실행 성공 여부를 알 수 없어 재시도 불가 → 완료.
 *   5) 그 외엔 check_retry_io로 재시도 가능성/지연을 판단해, 가능하면 재시도 큐에 넣고 반환.
 * complete 라벨에서는 accel_sequence를 비우고 CPL 상태로 bdev 완료를 보고한다.
 * 실행 컨텍스트: lib/nvme 완료 콜백(채널 스레드) → inline.
 *
 * 호출 체인:
 *   bdev_nvme_readv_done 등 완료 콜백 → [bdev_nvme_io_complete_nvme_status]
 *     → bdev_nvme_check_retry_io / bdev_nvme_queue_retry_io / __bdev_nvme_io_complete
 */
static inline void
bdev_nvme_io_complete_nvme_status(struct nvme_bdev_io *bio,
				  const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);    /* [한국어] 완료할 사용자 IO. */
	struct nvme_bdev_channel *nbdev_ch;                          /* [한국어] 재시도 큐가 필요할 때의 채널. */
	uint64_t delay_ms;                                          /* [한국어] 재시도 지연(ms). */

	/* [한국어] 이 경로는 일반 IO 전용 — admin류는 별도 완료 함수로 가야 함. */
	assert(!bdev_nvme_io_type_is_admin(bdev_io->type));

	/* [한국어] 성공: 경로 통계만 갱신하고 바로 완료. */
	if (spdk_likely(spdk_nvme_cpl_is_success(cpl))) {
		bdev_nvme_update_io_path_stat(bio);
		goto complete;
	}

	/* Update error counts before deciding if retry is needed.
	 * Hence, error counts may be more than the number of I/O errors.
	 */
	/* [한국어] (위 영어 주석) 재시도 판단 전에 에러 통계를 먼저 누적 — 재시도되는
	 * 에러도 카운트되므로 에러 카운트가 실제 IO 실패 수보다 클 수 있다. */
	bdev_nvme_update_nvme_error_stat(bdev_io, cpl);

	/* [한국어] DNR 비트(재시도 금지) / 사용자 abort / 재시도 한도 초과 → 더 시도 않고 완료. */
	if (cpl->status.dnr != 0 || spdk_nvme_cpl_is_aborted_by_request(cpl) ||
	    (g_opts.bdev_retry_count != -1 && bio->retry_count >= g_opts.bdev_retry_count)) {
		goto complete;
	}

	/* At this point we don't know whether the sequence was successfully executed or not, so we
	 * cannot retry the IO */
	/* [한국어] (위 영어 주석) accel 시퀀스가 걸린 IO는 실행 성공 여부 불확실 → 재시도 불가. */
	if (bdev_io->u.bdev.accel_sequence != NULL) {
		goto complete;
	}

	/* [한국어] 채널 컨텍스트 확보(재시도 큐 삽입 대상). */
	nbdev_ch = spdk_io_channel_get_ctx(spdk_bdev_io_get_io_channel(bdev_io));

	/* [한국어] 재시도 가능하면 지연 후 재시도 큐에 넣고 여기서 반환(완료 보고 안 함). */
	if (bdev_nvme_check_retry_io(bio, cpl, nbdev_ch, &delay_ms)) {
		bdev_nvme_queue_retry_io(nbdev_ch, bio, delay_ms);
		return;
	}

complete:
	bdev_io->u.bdev.accel_sequence = NULL;    /* [한국어] 완료 전 accel 시퀀스 참조 정리. */
	__bdev_nvme_io_complete(bdev_io, 0, cpl); /* [한국어] CPL 상태 그대로 bdev 코어로 완료. */
}

/*
 * [한국어]
 * bdev_nvme_io_complete - lib/nvme 제출 단계의 정수형 rc로 IO를 완료(또는 재시도)한다.
 *
 * @bio: 완료할 IO.
 * @rc: 제출/처리 결과 코드(0 성공, -ENOMEM, -ENXIO, 그 외 실패).
 *
 * CPL 없이 정수 rc만 있을 때 사용하는 완료 경로(주로 제출 자체 실패). rc 매핑:
 *   0      → SUCCESS.
 *   -ENOMEM → NOMEM(bdev 코어가 자원 회복 후 재제출하도록).
 *   -ENXIO  → 디바이스/경로 사라짐: 재시도 한도 내면 현재 경로를 버리고, 회복 가능
 *             경로가 있으면 1초 후 재시도 큐에 넣고 반환. 없으면 아래 default로 떨어짐.
 *   기타    → FAILED. R/W면 걸려 있던 accel 시퀀스를 abort하고 정리.
 * 실행 컨텍스트: 제출/완료 경로(채널 스레드) → inline.
 *
 * 호출 체인:
 *   bdev_nvme_*_done(rc 경로) → [bdev_nvme_io_complete]
 *     → bdev_nvme_queue_retry_io / __bdev_nvme_io_complete
 */
static inline void
bdev_nvme_io_complete(struct nvme_bdev_io *bio, int rc)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);    /* [한국어] 완료할 사용자 IO. */
	struct nvme_bdev_channel *nbdev_ch;                          /* [한국어] -ENXIO 재시도 시 채널. */
	enum spdk_bdev_io_status io_status;                         /* [한국어] bdev 코어에 보고할 상태. */

	/* [한국어] 일반 IO 전용 경로 — admin류는 별도 함수. */
	assert(!bdev_nvme_io_type_is_admin(bdev_io->type));

	switch (rc) {
	case 0:
		io_status = SPDK_BDEV_IO_STATUS_SUCCESS;    /* [한국어] 성공. */
		break;
	case -ENOMEM:
		io_status = SPDK_BDEV_IO_STATUS_NOMEM;      /* [한국어] 자원 부족 → bdev 코어가 후에 재제출. */
		break;
	case -ENXIO:
		/* [한국어] 디바이스/경로 소실: 재시도 한도 내면 경로를 옮겨 재시도 시도. */
		if (g_opts.bdev_retry_count == -1 || bio->retry_count < g_opts.bdev_retry_count) {
			nbdev_ch = spdk_io_channel_get_ctx(spdk_bdev_io_get_io_channel(bdev_io));

			/* [한국어] 죽은 경로 캐시/기억을 비워 다른 경로로 재선택되도록. */
			bdev_nvme_clear_current_io_path(nbdev_ch);
			bio->io_path = NULL;

			/* [한국어] 회복 가능 경로가 있으면 1초 후 재시도 예약 후 반환. */
			if (any_io_path_may_become_available(nbdev_ch)) {
				bdev_nvme_queue_retry_io(nbdev_ch, bio, 1000ULL);
				return;
			}
		}

	/* fallthrough */
	/* [한국어] (재시도 불가하면 그대로 실패 처리로 낙하) */
	default:
		/* [한국어] R/W였다면 진행 중이던 accel 시퀀스를 취소·정리(중간 상태 누수 방지). */
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ || bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			spdk_accel_sequence_abort(bdev_io->u.bdev.accel_sequence);
			bdev_io->u.bdev.accel_sequence = NULL;
		}
		io_status = SPDK_BDEV_IO_STATUS_FAILED;    /* [한국어] 최종 실패. */
		break;
	}

	__bdev_nvme_io_complete(bdev_io, io_status, NULL);    /* [한국어] bdev 레벨 상태로 완료(cpl=NULL). */
}

/*
 * [한국어]
 * bdev_nvme_admin_complete - admin류 IO(raw admin/reset/abort)를 정수 rc로 완료한다.
 *
 * @bio: 완료할 admin IO.
 * @rc: 결과 코드.
 *
 * 일반 IO와 달리 admin 경로는 멀티패스 재시도 로직이 없다(컨트롤러 단위 명령이라
 * 경로를 옮길 수 없음). rc를 단순히 SUCCESS/NOMEM/FAILED로 매핑해 완료한다.
 * 실행 컨텍스트: admin 완료 콜백(컨트롤러/채널 스레드) → inline.
 *
 * 호출 체인:
 *   admin 완료 콜백들 → [bdev_nvme_admin_complete] → __bdev_nvme_io_complete
 */
static inline void
bdev_nvme_admin_complete(struct nvme_bdev_io *bio, int rc)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);    /* [한국어] 완료할 admin IO. */
	enum spdk_bdev_io_status io_status;                         /* [한국어] 보고할 상태. */

	switch (rc) {
	case 0:
		io_status = SPDK_BDEV_IO_STATUS_SUCCESS;    /* [한국어] 성공. */
		break;
	case -ENOMEM:
		io_status = SPDK_BDEV_IO_STATUS_NOMEM;      /* [한국어] 자원 부족 → 후에 재제출. */
		break;
	case -ENXIO:
	/* fallthrough */
	default:
		io_status = SPDK_BDEV_IO_STATUS_FAILED;     /* [한국어] 그 외 모두 실패(재시도 없음). */
		break;
	}

	__bdev_nvme_io_complete(bdev_io, io_status, NULL);    /* [한국어] bdev 레벨 상태로 완료. */
}

/*
 * [한국어]
 * bdev_nvme_clear_io_path_caches_done - 모든 채널의 경로 캐시 비우기가 끝난 뒤 호출되는 완료 콜백.
 *
 * @nvme_ctrlr: 캐시 비우기를 수행한 컨트롤러.
 * @ctx: 미사용.
 * @status: 채널 순회 결과(미사용 — 캐시 비우기는 실패하지 않음).
 *
 * for_each_channel 순회가 끝나면 진행 플래그(io_path_cache_clearing)를 내리고, 순회를
 * 시작할 때 잡았던 컨트롤러 참조를 반납한다. 플래그/refcnt는 다른 스레드와 공유되므로
 * mutex로 보호한다.
 * 실행 컨텍스트: for_each_channel을 시작한 스레드(보통 컨트롤러 소유 스레드).
 *
 * 호출 체인:
 *   nvme_ctrlr_for_each_channel(완료) → [bdev_nvme_clear_io_path_caches_done]
 *     → nvme_ctrlr_put_ref
 */
static void
bdev_nvme_clear_io_path_caches_done(struct nvme_ctrlr *nvme_ctrlr,
				    void *ctx, int status)
{
	pthread_mutex_lock(&nvme_ctrlr->mutex);    /* [한국어] 플래그/refcnt 보호. */
	assert(nvme_ctrlr->io_path_cache_clearing == true);    /* [한국어] 진행 중이었음을 검증. */
	nvme_ctrlr->io_path_cache_clearing = false;    /* [한국어] 캐시 비우기 종료 표시. */
	nvme_ctrlr_put_ref(nvme_ctrlr);    /* [한국어] 시작 시 잡은 참조 반납(파괴 진행 가능). */
	pthread_mutex_unlock(&nvme_ctrlr->mutex);
}

/*
 * [한국어]
 * _bdev_nvme_clear_io_path_cache - 한 qpair에 매달린 모든 채널의 current_io_path 캐시를 비운다.
 *
 * @nvme_qpair: 캐시를 비울 대상 qpair.
 *
 * qpair의 io_path_list를 돌며, 살아 있는 채널(nbdev_ch != NULL)마다 캐시된 현재 경로를
 * 무효화한다. qpair 상태가 변했을 때(연결/끊김) 그 qpair를 쓰는 채널들이 다음 IO에서
 * 경로를 재선택하도록 강제하기 위함.
 * 실행 컨텍스트: 해당 qpair를 소유한 채널 스레드(for_each_channel 콜백 내부).
 *
 * 호출 체인:
 *   bdev_nvme_clear_io_path_cache / disconnected_qpair_cb → [_bdev_nvme_clear_io_path_cache]
 *     → bdev_nvme_clear_current_io_path
 */
static void
_bdev_nvme_clear_io_path_cache(struct nvme_qpair *nvme_qpair)
{
	struct nvme_io_path *io_path;    /* [한국어] qpair에 연결된 경로 순회 커서. */

	/* [한국어] 이 qpair를 쓰는 모든 경로를 순회. */
	TAILQ_FOREACH(io_path, &nvme_qpair->io_path_list, tailq) {
		/* [한국어] 이미 채널에서 분리된 경로(지연 free 대기)는 건너뜀. */
		if (io_path->nbdev_ch == NULL) {
			continue;
		}
		/* [한국어] 채널의 캐시된 현재 경로 무효화 → 다음 IO 때 재선택. */
		bdev_nvme_clear_current_io_path(io_path->nbdev_ch);
	}
}

/*
 * [한국어]
 * bdev_nvme_clear_io_path_cache - for_each_channel이 채널마다 호출하는 캐시 비우기 콜백.
 *
 * @i: 채널 순회 iterator.
 * @nvme_ctrlr: 대상 컨트롤러(미사용 — 시그니처용).
 * @ctrlr_ch: 현재 순회 중인 컨트롤러 채널.
 * @ctx: 미사용.
 *
 * 각 컨트롤러 채널의 qpair에 대해 경로 캐시를 비우고, 다음 채널로 순회를 이어간다.
 * for_each_channel은 채널마다 그 채널 소유 스레드로 메시지를 보내 콜백을 실행하므로,
 * 각 호출은 해당 채널의 lockless 자료구조를 안전하게 만진다(스레드 affinity).
 * 실행 컨텍스트: 각 컨트롤러 채널 소유 스레드.
 *
 * 호출 체인:
 *   nvme_ctrlr_for_each_channel → [bdev_nvme_clear_io_path_cache]
 *     → _bdev_nvme_clear_io_path_cache → nvme_ctrlr_for_each_channel_continue
 */
static void
bdev_nvme_clear_io_path_cache(struct nvme_ctrlr_channel_iter *i,
			      struct nvme_ctrlr *nvme_ctrlr,
			      struct nvme_ctrlr_channel *ctrlr_ch,
			      void *ctx)
{
	assert(ctrlr_ch->qpair != NULL);    /* [한국어] 채널엔 qpair가 있어야 함. */

	_bdev_nvme_clear_io_path_cache(ctrlr_ch->qpair);    /* [한국어] 이 채널 qpair의 경로 캐시 비우기. */

	nvme_ctrlr_for_each_channel_continue(i, 0);    /* [한국어] 다음 채널로 순회 진행. */
}

/*
 * [한국어]
 * bdev_nvme_clear_io_path_caches - 컨트롤러의 모든 채널에 걸쳐 경로 캐시 비우기를 시작한다.
 *
 * @nvme_ctrlr: 캐시를 비울 컨트롤러.
 *
 * ANA 변화/qpair 상태 변경 등으로 경로 선택을 재평가해야 할 때 호출한다. 컨트롤러가
 * 사용 불가하거나 이미 캐시 비우기가 진행 중이면 아무 일도 안 한다(중복 방지). 그렇지
 * 않으면 진행 플래그를 세우고 참조를 1 잡은 뒤 for_each_channel로 비동기 순회를 시작한다.
 * 참조를 잡는 이유: 순회가 끝나기 전 컨트롤러가 파괴되지 않도록(done 콜백에서 put).
 * 실행 컨텍스트: 컨트롤러 소유 스레드. 플래그/refcnt 공유 → mutex.
 *
 * 호출 체인:
 *   ANA 처리/qpair 상태 변경 → [bdev_nvme_clear_io_path_caches]
 *     → nvme_ctrlr_for_each_channel(...done)
 */
static void
bdev_nvme_clear_io_path_caches(struct nvme_ctrlr *nvme_ctrlr)
{
	pthread_mutex_lock(&nvme_ctrlr->mutex);    /* [한국어] 가용성/진행 플래그 검사 보호. */
	/* [한국어] 사용 불가하거나 이미 진행 중이면 중복 시작 방지. */
	if (!nvme_ctrlr_is_available(nvme_ctrlr) ||
	    nvme_ctrlr->io_path_cache_clearing) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return;
	}

	nvme_ctrlr->io_path_cache_clearing = true;    /* [한국어] 진행 중 표시. */
	nvme_ctrlr_get_ref(nvme_ctrlr);    /* [한국어] 순회 동안 파괴 방지용 참조 획득. */
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	/* [한국어] 채널별 콜백으로 캐시 비우기, 완료 시 done 콜백에서 참조 반납. */
	nvme_ctrlr_for_each_channel(nvme_ctrlr,
				    bdev_nvme_clear_io_path_cache,
				    NULL,
				    bdev_nvme_clear_io_path_caches_done);
}

/*
 * [한국어]
 * nvme_poll_group_get_qpair - poll group 안에서 lib/nvme qpair에 대응하는 nvme_qpair 래퍼를 찾는다.
 *
 * @group: 검색할 nvme_poll_group.
 * @qpair: lib/nvme의 실제 qpair 핸들(완료 콜백이 넘겨준 것).
 * @return: 매칭되는 nvme_qpair 래퍼, 없으면 NULL.
 *
 * lib/nvme의 disconnected/완료 콜백은 raw spdk_nvme_qpair 포인터만 넘겨주므로, 모듈의
 * 래퍼(nvme_qpair)를 역으로 찾아야 한다. poll group의 qpair_list를 선형 탐색한다.
 * 실행 컨텍스트: poll group을 소유한 채널 스레드.
 *
 * 호출 체인:
 *   bdev_nvme_disconnected_qpair_cb → [nvme_poll_group_get_qpair]
 */
static struct nvme_qpair *
nvme_poll_group_get_qpair(struct nvme_poll_group *group, struct spdk_nvme_qpair *qpair)
{
	struct nvme_qpair *nvme_qpair;    /* [한국어] 순회 커서/결과. */

	/* [한국어] 그룹의 모든 래퍼를 돌며 raw qpair 포인터가 일치하는 것을 찾는다. */
	TAILQ_FOREACH(nvme_qpair, &group->qpair_list, tailq) {
		if (nvme_qpair->qpair == qpair) {
			break;
		}
	}

	return nvme_qpair;    /* [한국어] 일치 래퍼(또는 미발견 시 NULL). */
}

/* [한국어] nvme_qpair 래퍼를 파괴하는 함수의 전방 선언(아래에서 정의). */
static void nvme_qpair_delete(struct nvme_qpair *nvme_qpair);

/*
 * [한국어]
 * nvme_ctrlr_channel_reset_finish - 한 컨트롤러 채널의 reset 단계를 마무리하고 다음 채널로 진행.
 *
 * @ctrlr_ch: reset을 마친 컨트롤러 채널.
 * @status: reset 결과(다음 단계로 전파).
 *
 * reset 시퀀스는 컨트롤러의 모든 채널을 순회하며 qpair를 재연결한다. 이 함수는 한
 * 채널의 재연결 폴러(connect_poller)를 해제하고, 보관해 둔 reset iterator로 다음 채널
 * 순회를 이어가게 한 뒤 iterator 참조를 비운다.
 * 실행 컨텍스트: 해당 컨트롤러 채널 소유 스레드.
 *
 * 호출 체인:
 *   reset connect 완료 경로 → [nvme_ctrlr_channel_reset_finish]
 *     → nvme_ctrlr_for_each_channel_continue
 */
static void
nvme_ctrlr_channel_reset_finish(struct nvme_ctrlr_channel *ctrlr_ch, int status)
{
	spdk_poller_unregister(&ctrlr_ch->connect_poller);    /* [한국어] 재연결 폴링 종료. */
	nvme_ctrlr_for_each_channel_continue(ctrlr_ch->reset_iter, status);    /* [한국어] 다음 채널로 reset 진행. */
	ctrlr_ch->reset_iter = NULL;    /* [한국어] iterator 참조 정리(이 채널의 reset 단계 종료). */
}

/*
 * [한국어]
 * bdev_nvme_disconnected_qpair_cb - lib/nvme이 qpair 끊김을 통지할 때 호출되는 콜백.
 *
 * @qpair: 끊긴 lib/nvme qpair 핸들.
 * @poll_group_ctx: 콜백 등록 시 넘긴 nvme_poll_group 포인터.
 *
 * spdk_nvme_poll_group_process_completions가 끊긴 qpair를 발견하면 이 콜백을 부른다.
 * 처리 분기:
 *   1) 래퍼를 못 찾으면(이미 정리됨) 무시.
 *   2) lib/nvme qpair를 free하고 래퍼의 핸들을 NULL로(재연결은 reset 경로가 담당).
 *   3) 이 qpair를 쓰던 채널들의 경로 캐시를 비운다.
 *   4) 컨트롤러 채널이 이미 삭제됐으면(ctrlr_ch==NULL) 래퍼만 파괴하고 종료.
 *   5) reset 중이 아니었으면(reset_iter==NULL) "예기치 못한 끊김" → failover로 복구.
 *   6) reset 시퀀스 도중이면: connect_poller가 남아 있으면 재연결 실패(status=-1),
 *      아니면 정상 끊김(status=0)으로 보고 reset 단계를 마무리한다.
 * 실행 컨텍스트: poll group 소유 채널 스레드(완료 폴링 중).
 *
 * 호출 체인:
 *   bdev_nvme_poll → spdk_nvme_poll_group_process_completions → [bdev_nvme_disconnected_qpair_cb]
 *     → nvme_qpair_delete / bdev_nvme_failover_ctrlr / nvme_ctrlr_channel_reset_finish
 */
static void
bdev_nvme_disconnected_qpair_cb(struct spdk_nvme_qpair *qpair, void *poll_group_ctx)
{
	struct nvme_poll_group *group = poll_group_ctx;    /* [한국어] qpair가 속한 poll group. */
	struct nvme_qpair *nvme_qpair;                    /* [한국어] raw qpair의 모듈 래퍼. */
	struct nvme_ctrlr *nvme_ctrlr;                    /* [한국어] qpair의 컨트롤러. */
	struct nvme_ctrlr_channel *ctrlr_ch;             /* [한국어] qpair의 컨트롤러 채널. */
	uint16_t qid;                                    /* [한국어] qpair ID(로그용). */
	int status;                                      /* [한국어] reset 단계로 전달할 결과. */

	/* [한국어] raw qpair → 래퍼 역매핑. 없으면 이미 정리된 것 → 무시. */
	nvme_qpair = nvme_poll_group_get_qpair(group, qpair);
	if (nvme_qpair == NULL) {
		return;
	}

	qid = spdk_nvme_qpair_get_id(qpair);    /* [한국어] 로그에 쓸 qpair ID 조회. */
	/* [한국어] 아직 살아 있는 lib/nvme qpair면 자원 해제 후 래퍼 핸들 비움. */
	if (nvme_qpair->qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(nvme_qpair->qpair);
		nvme_qpair->qpair = NULL;
	}

	/* [한국어] 이 qpair를 쓰던 채널들의 경로 캐시 무효화(끊긴 경로 재선택 방지). */
	_bdev_nvme_clear_io_path_cache(nvme_qpair);

	nvme_ctrlr = nvme_qpair->ctrlr;        /* [한국어] 컨트롤러 참조 복원. */
	ctrlr_ch = nvme_qpair->ctrlr_ch;       /* [한국어] 컨트롤러 채널 참조 복원. */

	/* In this case, ctrlr_channel is already deleted. */
	/* [한국어] (위 영어 주석) 컨트롤러 채널이 이미 삭제된 경우 → 래퍼만 파괴하고 종료. */
	if (ctrlr_ch == NULL) {
		NVME_CTRLR_INFOLOG(nvme_ctrlr,
				   NVME_QPAIR_LOG_FMT" was disconnected and freed. delete nvme_qpair.\n", qid, qpair);
		nvme_qpair_delete(nvme_qpair);
		return;
	}

	/* qpair was disconnected unexpectedly. Reset controller for recovery. */
	/* [한국어] (위 영어 주석) reset 중이 아닌데 끊김 = 예기치 못한 장애 → failover로 컨트롤러 복구. */
	if (ctrlr_ch->reset_iter == NULL) {
		NVME_CTRLR_INFOLOG(nvme_ctrlr,
				   NVME_QPAIR_LOG_FMT" was disconnected and freed. reset controller.\n", qid, qpair);
		bdev_nvme_failover_ctrlr(nvme_ctrlr);
		return;
	}

	/* We are in a full reset sequence. */
	/* [한국어] (위 영어 주석) reset 시퀀스 도중의 끊김: 재연결 폴러 유무로 성공/실패 구분. */
	if (ctrlr_ch->connect_poller != NULL) {
		/* [한국어] 재연결을 시도 중이었는데 끊김 = 연결 실패 → 시퀀스 중단(status=-1). */
		NVME_CTRLR_INFOLOG(nvme_ctrlr,
				   NVME_QPAIR_LOG_FMT" failed to connect. abort the reset ctrlr sequence.\n", qid, qpair);
		status = -1;
	} else {
		/* [한국어] 기존 qpair를 끊는 정상 단계였음 → 성공(status=0)으로 다음 단계 진행. */
		NVME_CTRLR_INFOLOG(nvme_ctrlr,
				   NVME_QPAIR_LOG_FMT" was disconnected and freed in a reset ctrlr sequence.\n", qid, qpair);
		status = 0;
	}

	nvme_ctrlr_channel_reset_finish(ctrlr_ch, status);    /* [한국어] 이 채널의 reset 단계 마무리. */
}

/*
 * [한국어]
 * bdev_nvme_check_io_qpairs - poll group의 모든 qpair를 점검해 실패한 것의 경로 캐시를 비운다.
 *
 * @group: 점검할 poll group.
 *
 * process_completions가 음수(트랜스포트 레벨 오류)를 반환하면, 어느 qpair가 망가졌는지
 * 알 수 없으므로 그룹 내 모든 qpair의 실패 사유를 확인해 실패한 것들의 경로 캐시를
 * 비운다(그 경로로 더 IO가 가지 않도록). 끊김/복구는 disconnected 콜백이 별도로 처리.
 * 실행 컨텍스트: poll group 소유 채널 스레드(폴링 직후).
 *
 * 호출 체인:
 *   bdev_nvme_poll(완료 음수 시) → [bdev_nvme_check_io_qpairs]
 *     → _bdev_nvme_clear_io_path_cache
 */
static void
bdev_nvme_check_io_qpairs(struct nvme_poll_group *group)
{
	struct nvme_qpair *nvme_qpair;    /* [한국어] qpair 순회 커서. */

	/* [한국어] 그룹의 모든 qpair를 점검. */
	TAILQ_FOREACH(nvme_qpair, &group->qpair_list, tailq) {
		/* [한국어] 미연결이거나 컨트롤러 채널이 없는 qpair는 점검 대상 아님. */
		if (nvme_qpair->qpair == NULL || nvme_qpair->ctrlr_ch == NULL) {
			continue;
		}

		/* [한국어] 실패 사유가 잡힌 qpair면 그 경로 캐시를 비워 더 이상 선택되지 않게. */
		if (spdk_nvme_qpair_get_failure_reason(nvme_qpair->qpair) !=
		    SPDK_NVME_QPAIR_FAILURE_NONE) {
			_bdev_nvme_clear_io_path_cache(nvme_qpair);
		}
	}
}

/*
 * [한국어]
 * bdev_nvme_poll - poll group의 IO 완료를 폴링하는 메인 poller(데이터 경로의 심장).
 *
 * @arg: nvme_poll_group 포인터.
 * @return: 완료가 있었으면 SPDK_POLLER_BUSY, 없으면 SPDK_POLLER_IDLE.
 *
 * SPDK polled-mode의 핵심: 인터럽트 대신 reactor가 이 poller를 무한 반복 호출해 CQ를
 * 폴링한다. spdk_nvme_poll_group_process_completions로 그룹 내 모든 qpair의 완료를
 * 한 번에 수확하고, 각 IO의 완료 콜백을 동기적으로 실행한다. collect_spin_stat가 켜져
 * 있으면 유효 완료가 없는 동안의 "헛도는 시간(spin)"을 측정해 효율 진단에 쓴다. 반환값이
 * 음수면 트랜스포트 오류로 보고 qpair 점검을 수행한다. polled-mode는 CPU를 계속 쓰는
 * 대신 인터럽트/문맥교환 지연을 없애 초저지연을 얻는 트레이드오프다.
 * 실행 컨텍스트: poll group을 소유한 채널 스레드(reactor poller).
 *
 * 호출 체인:
 *   SPDK reactor poller 루프 → [bdev_nvme_poll]
 *     → spdk_nvme_poll_group_process_completions(bdev_nvme_disconnected_qpair_cb)
 */
static int
bdev_nvme_poll(void *arg)
{
	struct nvme_poll_group *group = arg;    /* [한국어] 폴링할 poll group. */
	int64_t num_completions;                /* [한국어] 이번 폴링으로 수확한 완료 수(또는 음수 오류). */

	/* [한국어] spin 통계: 폴링 시작 틱을 처음 한 번 기록(유효 완료 전까지의 idle 측정 시작). */
	if (group->collect_spin_stat && group->start_ticks == 0) {
		group->start_ticks = spdk_get_ticks();
	}

	/* [한국어] 그룹 내 모든 qpair의 CQ를 폴링해 완료 콜백 실행(제한 0 = 가능한 만큼). */
	num_completions = spdk_nvme_poll_group_process_completions(group->group, 0,
			  bdev_nvme_disconnected_qpair_cb);
	if (group->collect_spin_stat) {
		if (num_completions > 0) {
			/* [한국어] 유효 완료 발생: 직전까지의 spin 구간을 누적하고 측정 리셋. */
			if (group->end_ticks != 0) {
				group->spin_ticks += (group->end_ticks - group->start_ticks);
				group->end_ticks = 0;
			}
			group->start_ticks = 0;
		} else {
			/* [한국어] 완료 없음: 헛도는 시점의 틱을 기록(다음 유효 완료 때 누적). */
			group->end_ticks = spdk_get_ticks();
		}
	}

	/* [한국어] 음수 = 트랜스포트 레벨 오류 → 어느 qpair가 실패했는지 점검. */
	if (spdk_unlikely(num_completions < 0)) {
		bdev_nvme_check_io_qpairs(group);
	}

	/* [한국어] 완료가 있었으면 BUSY(일 함), 없으면 IDLE → reactor의 효율적 sleep 판단 근거. */
	return num_completions > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

/* [한국어] admin 큐 폴링 poller 함수의 전방 선언(아래에서 정의). */
static int bdev_nvme_poll_adminq(void *arg);

/*
 * [한국어]
 * bdev_nvme_change_adminq_poll_period - admin 큐 폴링 주기를 변경(타이머 poller 재등록).
 *
 * @nvme_ctrlr: 대상 컨트롤러.
 * @new_period_us: 새 폴링 주기(마이크로초).
 *
 * admin 큐(AER, log page, reset 등)는 IO 큐만큼 자주 폴링할 필요가 없어 별도 타이머
 * poller로 처리한다. reset 진행 중에는 더 자주, 평상시엔 더 드물게 주기를 바꾼다.
 * interrupt mode에서는 폴링 대신 인터럽트가 admin 완료를 깨우므로 이 함수는 아무 일도
 * 하지 않는다(타이머 poller 자체가 없음).
 * 실행 컨텍스트: 컨트롤러 소유 스레드.
 *
 * 호출 체인:
 *   reset/connect 경로 → [bdev_nvme_change_adminq_poll_period] → SPDK_POLLER_REGISTER
 */
static void
bdev_nvme_change_adminq_poll_period(struct nvme_ctrlr *nvme_ctrlr, uint64_t new_period_us)
{
	/* [한국어] interrupt mode면 타이머 폴링이 없으므로 무시. */
	if (spdk_interrupt_mode_is_enabled()) {
		return;
	}

	/* [한국어] 기존 타이머 poller 해제 후 새 주기로 재등록(주기 변경 = 해제+재등록). */
	spdk_poller_unregister(&nvme_ctrlr->adminq_timer_poller);

	nvme_ctrlr->adminq_timer_poller = SPDK_POLLER_REGISTER(bdev_nvme_poll_adminq,
					  nvme_ctrlr, new_period_us);
}

/*
 * [한국어]
 * bdev_nvme_poll_adminq - admin 큐 완료를 폴링하고 끊김/실패를 감지·처리하는 타이머 poller.
 *
 * @arg: nvme_ctrlr 포인터.
 * @return: 완료가 없으면 IDLE, 있거나 오류면 BUSY.
 *
 * admin 큐의 완료(AER, log page, identify 등)를 처리한다. 결과 분기:
 *   - rc < 0(admin 큐 끊김): disconnected_cb가 설정돼 있으면(의도된 disconnect, 예: reset
 *     중) 폴링 주기를 평상시로 복원하고 그 콜백을 호출해 다음 단계로 진행. 콜백이 없으면
 *     예기치 못한 끊김이므로 failover로 복구.
 *   - rc >= 0이지만 admin qpair 실패 사유가 있으면: 경로 캐시를 비워 IO를 다른 경로로.
 * 실행 컨텍스트: 컨트롤러 소유 스레드의 타이머 poller(non-interrupt mode).
 *
 * 호출 체인:
 *   SPDK reactor 타이머 → [bdev_nvme_poll_adminq]
 *     → spdk_nvme_ctrlr_process_admin_completions / disconnected_cb / bdev_nvme_failover_ctrlr
 */
static int
bdev_nvme_poll_adminq(void *arg)
{
	int32_t rc;                                   /* [한국어] admin 완료 처리 결과(음수=끊김). */
	struct nvme_ctrlr *nvme_ctrlr = arg;          /* [한국어] 폴링 대상 컨트롤러. */
	nvme_ctrlr_disconnected_cb disconnected_cb;   /* [한국어] 의도된 disconnect 후속 콜백. */

	assert(nvme_ctrlr != NULL);

	/* [한국어] admin 큐의 완료를 처리(AER/log/identify 등 콜백 실행). */
	rc = spdk_nvme_ctrlr_process_admin_completions(nvme_ctrlr->ctrlr);
	if (rc < 0) {
		/* [한국어] admin 큐 끊김: 등록된 disconnect 콜백을 꺼내 한 번만 호출하도록 비움. */
		disconnected_cb = nvme_ctrlr->disconnected_cb;
		nvme_ctrlr->disconnected_cb = NULL;

		if (disconnected_cb != NULL) {
			/* [한국어] 의도된 disconnect(reset 단계 등): 주기 복원 후 후속 콜백 진행. */
			bdev_nvme_change_adminq_poll_period(nvme_ctrlr,
							    g_opts.nvme_adminq_poll_period_us);
			disconnected_cb(nvme_ctrlr);
		} else {
			/* [한국어] 예기치 못한 끊김: failover로 컨트롤러 복구 시도. */
			bdev_nvme_failover_ctrlr(nvme_ctrlr);
		}
	} else if (spdk_nvme_ctrlr_get_admin_qp_failure_reason(nvme_ctrlr->ctrlr) !=
		   SPDK_NVME_QPAIR_FAILURE_NONE) {
		/* [한국어] 끊기진 않았지만 admin qpair 실패 사유가 잡힘 → 경로 캐시 비우기. */
		bdev_nvme_clear_io_path_caches(nvme_ctrlr);
	}

	/* [한국어] 완료 없음(rc==0)이면 IDLE, 그 외(완료 있음/오류)면 BUSY. */
	return rc == 0 ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * nvme_bdev_free - nvme_bdev 객체와 부속 자원을 최종 해제하는 io_device unregister 콜백.
 *
 * @io_device: spdk_io_device_unregister에 등록됐던 nvme_bdev 포인터.
 *
 * 모든 IO 채널이 정리된 안전한 시점에 SPDK 프레임워크가 호출한다. mutex 파괴 + 이름
 * 문자열 + 에러 통계 + 객체 본체를 차례로 해제한다.
 * 실행 컨텍스트: app 스레드(io_device unregister 완료 콜백).
 *
 * 호출 체인:
 *   bdev_nvme_destruct → spdk_io_device_unregister(채널 정리 후) → [nvme_bdev_free]
 */
static void
nvme_bdev_free(void *io_device)
{
	struct nvme_bdev *nbdev = io_device;    /* [한국어] 해제할 논리 bdev. */

	pthread_mutex_destroy(&nbdev->mutex);    /* [한국어] 필드 보호용 mutex 파괴. */
	free(nbdev->disk.name);                  /* [한국어] bdev 이름 문자열 해제. */
	free(nbdev->err_stat);                   /* [한국어] NVMe 에러 통계 배열 해제(없으면 NULL). */
	free(nbdev);                             /* [한국어] 객체 본체 해제. */
}

/*
 * [한국어]
 * bdev_nvme_destruct - bdev fn_table::destruct 콜백. nvme_bdev를 해체하기 시작한다.
 *
 * @ctx: 해체할 nvme_bdev 포인터.
 * @return: 항상 0(비동기 해제이므로 즉시 성공 보고; 실제 free는 채널 정리 후).
 *
 * spdk_bdev_unregister가 마지막 참조를 떨궜을 때 bdev 코어가 호출한다. 이 bdev에 묶인
 * 모든 namespace의 역참조(nvme_ns->bdev)를 끊고, 각 ns가 여전히 이 bdev의 마지막 참조면
 * 즉시 삭제한다. 단, reconnect 후 같은 NSID로 새 ns 객체가 생겼을 수 있는데, 그 경우
 * 컨트롤러의 현재 ns가 다른 객체이므로 원본만 삭제하고 새 것은 건드리지 않는다(영어
 * 주석 참조). 마지막에 nbdev_ctrlr의 bdevs 리스트에서 빼고, io_device를 unregister해
 * 모든 채널이 정리된 뒤 nvme_bdev_free가 최종 해제하도록 한다.
 * 실행 컨텍스트: app 스레드 전용(전역 리스트/컨트롤러 그룹을 만지므로 assert로 강제).
 *
 * 호출 체인:
 *   spdk_bdev_unregister → bdev 코어 → [bdev_nvme_destruct]
 *     → nvme_ns_delete / spdk_io_device_unregister(nvme_bdev_free)
 */
static int
bdev_nvme_destruct(void *ctx)
{
	struct nvme_bdev *nbdev = ctx;                  /* [한국어] 해체 대상 논리 bdev. */
	struct nvme_ns *nvme_ns, *tmp_nvme_ns;          /* [한국어] ns 순회 커서 + 제거 안전용. */

	/* [한국어] 전역/컨트롤러 그룹 상태를 변경하므로 반드시 app 스레드에서만 실행. */
	assert(spdk_thread_is_app_thread(NULL));

	NVME_BDEV_DEBUGLOG(nbdev, null_ctrlr, "destructing bdev\n");
	/* [한국어] DTrace 프로브: 어떤 컨트롤러/NSID의 bdev가 해체되는지 추적. */
	SPDK_DTRACE_PROBE2(bdev_nvme_destruct, nbdev->nbdev_ctrlr->name, nbdev->nsid);

	/* [한국어] 이 bdev를 구성하던 모든 namespace(=path)를 순회 정리. */
	TAILQ_FOREACH_SAFE(nvme_ns, &nbdev->nvme_ns_list, tailq, tmp_nvme_ns) {
		nvme_ns->bdev = NULL;    /* [한국어] ns → bdev 역참조 끊기. */

		assert(nvme_ns->id > 0);    /* [한국어] 유효 NSID 검증(0은 무효). */

		/* A new namespace object with the same NSID may have been created after reconnect.
		 * In that case, ignore the new one and continue destroying the original namespace.
		 */
		/* [한국어] (위 영어 주석) reconnect로 같은 NSID의 새 ns가 생겼을 수 있다.
		 * 컨트롤러의 현재 ns가 이 객체와 같을 때만 "마지막 참조"이므로 즉시 삭제하고,
		 * 다르면(새 객체로 교체됨) 원본은 depopulate 완료 시점까지 삭제를 미룬다. */
		if (nvme_ctrlr_get_ns(nvme_ns->ctrlr, nvme_ns->id) != nvme_ns) {
			NVME_NS_DEBUGLOG(nvme_ns, "ns free with the last reference to nbdev\n");
			TAILQ_REMOVE(&nbdev->nvme_ns_list, nvme_ns, tailq);
			nvme_ns_delete(nvme_ns);
		} else {
			NVME_NS_DEBUGLOG(nvme_ns, "defer ns free until depopulate is done\n");
		}
	}

	/* [한국어] 컨트롤러 그룹의 bdevs 리스트에서 이 bdev 제거. */
	TAILQ_REMOVE(&nbdev->nbdev_ctrlr->bdevs, nbdev, tailq);
	/* [한국어] io_device 해제 요청 — 모든 채널 정리 후 nvme_bdev_free가 최종 free. */
	spdk_io_device_unregister(nbdev, nvme_bdev_free);

	return 0;    /* [한국어] 비동기 해제 시작 성공(실제 완료는 콜백에서). */
}

/*
 * [한국어]
 * bdev_nvme_create_qpair - 한 I/O 채널이 사용할 NVMe I/O qpair(SQ+CQ 한 쌍)를 생성·연결한다.
 *
 * @nvme_qpair: 이 SPDK thread(=I/O 채널) 전용으로 만들어진 nvme_qpair 래퍼.
 *              내부의 group(=poll group)은 이미 채널 생성 시점에 설정되어 있어야 한다.
 * @return: 0=성공(연결 시작됨, 실제 연결 완료는 poll group 폴링에서 비동기 완료),
 *          음수=qpair 할당/poll group 추가/connect 중 실패.
 *
 * 왜 필요한가: SPDK는 코어(=spdk_thread=reactor)마다 독립된 NVMe qpair를 둬서 lock 없이
 * I/O를 발행한다. 채널이 처음 만들어지거나 reset 후 재연결될 때 이 함수로 qpair를 새로 띄운다.
 * 동작: (1) 컨트롤러 기본 qpair opts를 복사하고 create_only=true로 "생성만, 연결은 분리" 모드 설정,
 * (2) 인터럽트 모드가 아니면 async_mode/delay_cmd_submit 같은 폴링 최적화 활성화,
 * (3) alloc_io_qpair로 qpair 객체 생성, (4) 이 채널의 poll group에 등록, (5) connect_io_qpair로
 * Fabrics Connect(또는 PCIe SQ/CQ 활성)를 비동기 개시. create_only로 alloc과 connect를 분리해야
 * poll group이 연결 진행 상태를 폴링으로 관찰할 수 있다.
 * 실행 컨텍스트: 해당 채널을 소유한 I/O spdk_thread. qpair는 그 스레드에만 묶이므로 lockless.
 *
 * 호출 체인:
 *   bdev_nvme_create_ctrlr_channel_cb / 재연결 경로 → [bdev_nvme_create_qpair]
 *     → spdk_nvme_ctrlr_alloc_io_qpair → spdk_nvme_poll_group_add → spdk_nvme_ctrlr_connect_io_qpair
 */
static int
bdev_nvme_create_qpair(struct nvme_qpair *nvme_qpair)
{
	struct nvme_ctrlr *nvme_ctrlr;                  /* [한국어] qpair가 속한 NVMe 컨트롤러 래퍼. */
	struct spdk_nvme_io_qpair_opts opts;            /* [한국어] qpair 생성 옵션(큐 깊이/모드 등). */
	struct spdk_nvme_qpair *qpair;                  /* [한국어] 드라이버가 반환할 실제 qpair 핸들. */
	int rc;                                         /* [한국어] 하위 호출 반환 코드 임시 저장. */

	nvme_ctrlr = nvme_qpair->ctrlr;                 /* [한국어] 래퍼에서 소속 컨트롤러 역참조. */

	/* [한국어] 컨트롤러가 권장하는 기본 qpair 옵션을 opts에 채운다(큐 깊이/벡터 등). */
	spdk_nvme_ctrlr_get_default_io_qpair_opts(nvme_ctrlr->ctrlr, &opts, sizeof(opts));
	opts.create_only = true;                        /* [한국어] alloc 시점엔 연결하지 말고 핸들만 생성 — connect를 분리해 poll group이 진행을 폴링. */
	/* In interrupt mode qpairs must be created in sync mode, else it will never be connected.
	 * delay_cmd_submit must be false as in interrupt mode requests cannot be submitted in
	 * completion context.
	 */
	/* [한국어] (위 영어 주석) 인터럽트 모드에서는 완료 컨텍스트에서 명령 제출이 불가하므로
	 * async/delay 최적화를 끈다. 폴링 모드일 때만 아래 두 최적화를 켠다. */
	if (!spdk_interrupt_mode_is_enabled()) {
		opts.async_mode = true;                 /* [한국어] 비동기 SQE 제출 허용(폴링 컨텍스트에서 안전). */
		opts.delay_cmd_submit = g_opts.delay_cmd_submit; /* [한국어] 여러 SQE를 모아 doorbell 1회로 batch(MMIO 절감). */
	}
	/* [한국어] 채널 큐 깊이는 사용자 설정과 드라이버 기본 중 큰 값을 채택(요청 큐 부족 방지). */
	opts.io_queue_requests = spdk_max(g_opts.io_queue_requests, opts.io_queue_requests);
	g_opts.io_queue_requests = opts.io_queue_requests; /* [한국어] 합의된 값을 전역에 되돌려 이후 qpair들과 일관성 유지. */

	/* [한국어] 실제 NVMe I/O qpair 생성 — SQ/CQ 메모리 할당(연결은 create_only로 미룸). */
	qpair = spdk_nvme_ctrlr_alloc_io_qpair(nvme_ctrlr->ctrlr, &opts, sizeof(opts));
	if (qpair == NULL) {                            /* [한국어] 큐 자원 고갈/컨트롤러 불가 시 NULL. */
		return -1;                              /* [한국어] 호출자(채널 생성)가 채널 구성 실패로 처리. */
	}

	/* [한국어] DTrace 프로브: 어떤 컨트롤러/큐ID/스레드에서 qpair가 생성되는지 추적. */
	SPDK_DTRACE_PROBE3(bdev_nvme_create_qpair, nvme_ctrlr->nbdev_ctrlr->name,
			   spdk_nvme_qpair_get_id(qpair), spdk_thread_get_id(spdk_get_thread()));

	assert(nvme_qpair->group != NULL);              /* [한국어] poll group은 채널 생성 시 반드시 선설정. */

	/* [한국어] qpair를 이 채널의 poll group에 추가 — 이후 group 폴링이 이 큐 완료를 수거. */
	rc = spdk_nvme_poll_group_add(nvme_qpair->group->group, qpair);
	if (rc != 0) {                                  /* [한국어] poll group 등록 실패. */
		NVME_QPAIR_ERRLOG(nvme_qpair, "Unable to begin polling on NVMe Channel.\n");
		goto err;                               /* [한국어] 생성한 qpair를 해제하고 빠져나간다. */
	}

	/* [한국어] Fabrics Connect 캡슐 전송(또는 PCIe SQ/CQ 활성) 개시 — 완료는 비동기. */
	rc = spdk_nvme_ctrlr_connect_io_qpair(nvme_ctrlr->ctrlr, qpair);
	if (rc != 0) {                                  /* [한국어] connect 개시 실패(전송 오류/상태 불가). */
		NVME_QPAIR_ERRLOG(nvme_qpair, "Unable to connect I/O qpair.\n");
		goto err;                               /* [한국어] qpair 해제 후 실패 반환. */
	}

	nvme_qpair->qpair = qpair;                      /* [한국어] 성공 시 래퍼에 실제 핸들 연결(이후 I/O 발행에 사용). */

	/* [한국어] 자동 failback이 켜져 있으면, 새 path가 살아났으니 채널들의 io_path 캐시를 무효화. */
	if (!g_opts.disable_auto_failback) {
		_bdev_nvme_clear_io_path_cache(nvme_qpair);
	}

	NVME_QPAIR_INFOLOG(nvme_qpair, "Connecting qpair started.\n");
	return 0;                                       /* [한국어] 연결 개시 성공(완료는 poll group 폴링에서). */

err:
	spdk_nvme_ctrlr_free_io_qpair(qpair);           /* [한국어] 실패 정리: 할당된 qpair 메모리/큐 해제. */

	return rc;                                       /* [한국어] 실패 코드 전달. */
}

/* [한국어] 전방 선언: pending reset I/O 하나에 reset 결과를 통지하는 콜백(아래에서 정의). */
static void bdev_nvme_reset_io_continue(void *cb_arg, int rc);

/*
 * [한국어]
 * bdev_nvme_complete_pending_resets - reset 완료 시, 그동안 대기 큐에 쌓인 reset 요청들을 일괄 완료한다.
 *
 * @nvme_ctrlr: reset이 막 끝난 컨트롤러. pending_resets 리스트에 중복 reset 요청들이 들어 있다.
 * @success: 방금 끝난 reset의 성공 여부. 대기 중이던 모든 요청에 동일 결과를 전파한다.
 * @return: 없음.
 *
 * 왜 필요한가: 한 컨트롤러에 reset이 진행 중일 때 또 다른 bdev_reset 요청이 오면 즉시 실행하지 않고
 * pending_resets에 큐잉한다(중복 reset 방지). 진행 중이던 reset이 끝나면 그 결과를 대기 요청들에
 * 그대로 돌려준다. 동작: 리스트가 빌 때까지 head를 꺼내며 각 bio에 bdev_nvme_reset_io_continue를
 * 호출해 성공이면 0, 실패면 -1을 통지한다.
 * 실행 컨텍스트: app(메인) spdk_thread — reset 완료 경로(bdev_nvme_reset_ctrlr_complete)에서 호출.
 *
 * 호출 체인:
 *   bdev_nvme_reset_ctrlr_complete → [bdev_nvme_complete_pending_resets] → bdev_nvme_reset_io_continue
 */
static void
bdev_nvme_complete_pending_resets(struct nvme_ctrlr *nvme_ctrlr, bool success)
{
	int rc = 0;                                     /* [한국어] 대기 요청들에 통지할 결과 코드(기본 성공). */
	struct nvme_bdev_io *bio;                       /* [한국어] 큐에서 꺼낸 reset 요청 I/O. */

	if (!success) {                                 /* [한국어] reset이 실패했으면 모든 대기 요청도 실패 처리. */
		rc = -1;
	}

	/* [한국어] pending_resets가 빌 때까지 head부터 하나씩 꺼내 결과 통지. */
	while (!TAILQ_EMPTY(&nvme_ctrlr->pending_resets)) {
		bio = TAILQ_FIRST(&nvme_ctrlr->pending_resets); /* [한국어] 가장 오래 기다린 요청부터. */
		TAILQ_REMOVE(&nvme_ctrlr->pending_resets, bio, retry_link); /* [한국어] 큐에서 제거(중복 완료 방지). */

		bdev_nvme_reset_io_continue(bio, rc);   /* [한국어] 해당 reset I/O에 결과 전달 → bdev_io 완료로 이어짐. */
	}
}

/* This function marks the current trid as failed by storing the current ticks
 * and then sets the next trid to the active trid within a controller if exists.
 *
 * The purpose of the boolean return value is to request the caller to disconnect
 * the current trid now to try connecting the next trid.
 */
/*
 * [한국어]
 * bdev_nvme_failover_trid - 현재 활성 경로(trid)를 실패로 표시하고 다음 대체 경로로 전환한다.
 *
 * @nvme_ctrlr: 다중 경로(trid 리스트)를 가진 컨트롤러. 첫 trid가 현재 활성 경로여야 한다.
 * @remove: true면 실패한 trid를 리스트에서 제거(영구 삭제), false면 라운드로빈용으로 리스트 끝으로 이동.
 * @start: failover 시퀀스의 첫 진입인지 여부. true면 backoff 무시하고 다음 trid를 즉시 시도.
 * @return: true=호출자가 지금 즉시 현재 trid를 disconnect하고 다음 trid 연결을 시도해야 함,
 *          false=대체 경로가 없거나 아직 backoff 대기 중이라 지금 전환하지 않음.
 *
 * 왜 필요한가: NVMe-oF 다중 경로(multipath) 환경에서 한 네트워크 경로가 죽으면 다른 traddr/trsvcid로
 * 자동 전환(failover)해야 한다. (영어 주석 참고) 반환 bool은 "지금 끊고 다음 경로로 붙어라"는 신호다.
 * 동작: (1) 현재 trid의 last_failed_tsc에 현재 tsc 기록(=실패 표시), (2) 다음 trid가 없으면 false,
 * (3) reset 시퀀스 중 재시도 비활성(reconnect_delay=0, start 아님)이면 false, (4) 컨트롤러를 fail로
 * 강제하고 active_path_id를 다음 trid로 교체 후 드라이버에 set_trid, (5) 이전 trid를 remove하거나
 * 리스트 끝으로 회전(라운드로빈), (6) start이거나 다음 trid가 한 번도 실패하지 않았으면 즉시 시도(true),
 * (7) backoff(reconnect_delay_sec)가 충분히 지났으면 true, 아니면 false.
 * 실행 컨텍스트: app spdk_thread, nvme_ctrlr->mutex 보유 상태에서 호출됨.
 *
 * 호출 체인:
 *   bdev_nvme_reset_ctrlr_complete / bdev_nvme_failover_ctrlr → [bdev_nvme_failover_trid]
 */
static bool
bdev_nvme_failover_trid(struct nvme_ctrlr *nvme_ctrlr, bool remove, bool start)
{
	struct spdk_nvme_path_id *path_id, *next_path; /* [한국어] 현재 경로와 다음 후보 경로. */
	int rc __attribute__((unused));                 /* [한국어] set_trid 반환(릴리스 빌드에선 assert만 사용). */

	path_id = TAILQ_FIRST(&nvme_ctrlr->trids);      /* [한국어] 리스트 head = 현재 활성 trid. */
	assert(path_id);                                /* [한국어] 최소 1개 경로는 존재해야 함. */
	assert(path_id == nvme_ctrlr->active_path_id);  /* [한국어] head가 곧 활성 경로라는 불변식 확인. */
	next_path = TAILQ_NEXT(path_id, link);          /* [한국어] 전환 후보(다음 경로) — 없을 수 있음. */

	/* Update the last failed time. It means the trid is failed if its last
	 * failed time is non-zero.
	 */
	/* [한국어] (위 영어) last_failed_tsc != 0 이면 그 경로는 "실패"로 간주. 현재 시각 기록. */
	path_id->last_failed_tsc = spdk_get_ticks();

	if (next_path == NULL) {                        /* [한국어] 대체 경로가 없으면 전환 불가. */
		/* There is no alternate trid within a controller. */
		return false;                           /* [한국어] 호출자는 같은 경로 재시도 또는 reconnect 대기. */
	}

	if (!start && nvme_ctrlr->opts.reconnect_delay_sec == 0) {
		/* Connect is not retried in a controller reset sequence. Connecting
		 * the next trid will be done by the next bdev_nvme_failover_ctrlr() call.
		 */
		/* [한국어] reconnect 지연이 0이면 reset 시퀀스 안에서 재연결을 시도하지 않는다.
		 * 다음 trid 연결은 다음 failover 호출이 담당하므로 지금은 false. */
		return false;
	}

	assert(path_id->trid.trtype != SPDK_NVME_TRANSPORT_PCIE); /* [한국어] PCIe는 단일 경로 — failover는 Fabrics만. */

	NVME_CTRLR_NOTICELOG(nvme_ctrlr, "Start failover to %s:%s\n", next_path->trid.traddr,
			     next_path->trid.trsvcid); /* [한국어] 어느 주소:포트로 전환하는지 로그. */
	spdk_nvme_ctrlr_fail(nvme_ctrlr->ctrlr);        /* [한국어] 현재 경로의 컨트롤러를 강제 fail — 진행 중 I/O 정리. */
	nvme_ctrlr->active_path_id = next_path;         /* [한국어] 활성 경로 포인터를 다음 trid로 갱신. */
	rc = spdk_nvme_ctrlr_set_trid(nvme_ctrlr->ctrlr, &next_path->trid); /* [한국어] 드라이버에 새 전송 주소 알림. */
	assert(rc == 0);                                /* [한국어] fail 상태에서 set_trid는 항상 성공해야 함. */
	TAILQ_REMOVE(&nvme_ctrlr->trids, path_id, link); /* [한국어] 이전 경로를 리스트에서 분리(아래에서 재배치/삭제). */
	if (!remove) {
		/** Shuffle the old trid to the end of the list and use the new one.
		 * Allows for round robin through multiple connections.
		 */
		/* [한국어] (위 영어) 이전 trid를 리스트 끝으로 회전 — 여러 경로 라운드로빈 순환 유지. */
		TAILQ_INSERT_TAIL(&nvme_ctrlr->trids, path_id, link);
	} else {
		free(path_id);                          /* [한국어] remove면 이전 경로를 영구 삭제. */
	}

	if (start || next_path->last_failed_tsc == 0) {
		/* bdev_nvme_failover_ctrlr() is just called or the next trid is not failed
		 * or used yet. Try the next trid now.
		 */
		/* [한국어] 첫 진입이거나 다음 경로가 한 번도 실패한 적 없으면 backoff 없이 즉시 시도. */
		return true;
	}

	if (spdk_get_ticks() > next_path->last_failed_tsc + spdk_get_ticks_hz() *
	    nvme_ctrlr->opts.reconnect_delay_sec) {
		/* Enough backoff passed since the next trid failed. Try the next trid now. */
		/* [한국어] 다음 경로가 마지막으로 실패한 뒤 reconnect_delay_sec만큼 충분히 지났으면 재시도. */
		return true;
	}

	/* The next trid will be tried after reconnect_delay_sec seconds. */
	return false;                                   /* [한국어] 아직 backoff 중 — 나중에 재시도. */
}

/*
 * [한국어]
 * bdev_nvme_check_ctrlr_loss_timeout - 컨트롤러 손실 타임아웃(ctrlr_loss_timeout_sec) 경과 여부 판정.
 *
 * @nvme_ctrlr: reset/reconnect 재시도 중인 컨트롤러. reset_start_tsc 기준으로 경과를 측정.
 * @return: true=손실 타임아웃 초과(이제 컨트롤러를 영구 삭제해야 함), false=아직 유예 내(또는 무제한).
 *
 * 왜 필요한가: NVMe-oF 연결이 끊긴 뒤 무한정 재연결을 시도하지 않고, 사용자가 지정한 시간이 지나면
 * 컨트롤러를 포기(삭제)해야 한다. 동작: timeout이 0이거나 -1(무제한)이면 항상 false, 그 외에는
 * reset 시작 이후 경과 초(elapsed)를 손실 타임아웃과 비교.
 * 실행 컨텍스트: app spdk_thread(reset 완료/재연결 폴러 경로). spdk_get_ticks_hz()로 tsc→초 변환.
 *
 * 호출 체인:
 *   bdev_nvme_check_op_after_reset / 재연결 폴러 → [bdev_nvme_check_ctrlr_loss_timeout]
 */
static bool
bdev_nvme_check_ctrlr_loss_timeout(struct nvme_ctrlr *nvme_ctrlr)
{
	uint32_t elapsed;                               /* [한국어] reset 시작 이후 경과 시간(초). */

	/* [한국어] 0(즉시 포기 안 함) 또는 -1(무제한 재시도)이면 손실 타임아웃 미적용. */
	if (nvme_ctrlr->opts.ctrlr_loss_timeout_sec == 0 ||
	    nvme_ctrlr->opts.ctrlr_loss_timeout_sec == -1) {
		return false;
	}

	assert(nvme_ctrlr->opts.ctrlr_loss_timeout_sec >= 0); /* [한국어] 위 분기 이후엔 양수만 남음. */
	/* [한국어] (현재 tsc - reset 시작 tsc)를 tsc 주파수로 나눠 경과 초 계산. */
	elapsed = (spdk_get_ticks() - nvme_ctrlr->reset_start_tsc) / spdk_get_ticks_hz();
	if (elapsed >= (uint32_t)nvme_ctrlr->opts.ctrlr_loss_timeout_sec) {
		return true;                            /* [한국어] 유예 초과 → 컨트롤러 포기/삭제. */
	} else {
		return false;                           /* [한국어] 아직 유예 내 → 계속 재시도. */
	}
}

/*
 * [한국어]
 * bdev_nvme_check_fast_io_fail_timeout - fast I/O fail 타임아웃 경과 여부 판정.
 *
 * @nvme_ctrlr: reset 중인 컨트롤러. reset_start_tsc 기준 경과를 측정.
 * @return: true=fast_io_fail 타임아웃 초과(대기 중 I/O를 빨리 실패시켜야 함), false=아직 유예 내(또는 비활성).
 *
 * 왜 필요한가: 컨트롤러 복구를 끝까지 기다리지 않고, 더 짧은 fast_io_fail_timeout_sec이 지나면
 * 큐에 묶인 I/O를 즉시 실패로 돌려 상위 애플리케이션의 지연을 제한한다(ctrlr_loss_timeout보다 짧게 설정).
 * 동작: timeout이 0이면 비활성(false), 그 외 reset 이후 경과 초를 비교.
 * 실행 컨텍스트: app spdk_thread(reset 완료 경로).
 *
 * 호출 체인:
 *   bdev_nvme_reset_ctrlr_complete → [bdev_nvme_check_fast_io_fail_timeout]
 */
static bool
bdev_nvme_check_fast_io_fail_timeout(struct nvme_ctrlr *nvme_ctrlr)
{
	uint32_t elapsed;                               /* [한국어] reset 시작 이후 경과 초. */

	if (nvme_ctrlr->opts.fast_io_fail_timeout_sec == 0) { /* [한국어] 0이면 fast-fail 비활성. */
		return false;
	}

	/* [한국어] reset 시작 이후 경과 초 계산. */
	elapsed = (spdk_get_ticks() - nvme_ctrlr->reset_start_tsc) / spdk_get_ticks_hz();
	if (elapsed >= nvme_ctrlr->opts.fast_io_fail_timeout_sec) {
		return true;                            /* [한국어] 임계 초과 → 대기 I/O를 빠르게 실패. */
	} else {
		return false;                           /* [한국어] 아직 유예 내. */
	}
}

/* [한국어] 전방 선언: reset 시퀀스 종료 시 상태 정리/후속 동작을 수행하는 함수(아래 정의). */
static void bdev_nvme_reset_ctrlr_complete(struct nvme_ctrlr *nvme_ctrlr, bool success);

/*
 * [한국어]
 * nvme_ctrlr_disconnect - 컨트롤러 연결을 끊고, 끊김 완료 시 실행할 콜백을 등록한다.
 *
 * @nvme_ctrlr: 끊을 컨트롤러. reset/failover/재연결 시퀀스의 한 단계로 호출됨.
 * @cb_fn: 실제로 disconnect가 완료된 뒤(adminq 폴링으로 비동기 확인) 호출할 콜백
 *         (예: bdev_nvme_reconnect_ctrlr, bdev_nvme_start_reconnect_delay_timer).
 * @return: 없음(실패 시 즉시 reset 실패로 마감).
 *
 * 왜 필요한가: NVMe 컨트롤러 disconnect는 즉시 끝나지 않고 adminq를 폴링하며 비동기로 완료된다.
 * 그래서 "끊긴 다음 무엇을 할지"를 콜백으로 예약해 두고, 완료 시점에 그 콜백이 호출되도록 한다.
 * 동작: (1) spdk_nvme_ctrlr_disconnect 시도, (2) 실패(이미 resetting/removed)면 reset을 즉시 실패
 * 처리, (3) 성공 시 disconnected_cb에 콜백 저장, (4) 더 빠른 완료 감지를 위해 adminq 폴링 주기를
 * 0(가능한 한 자주)으로 단축.
 * 실행 컨텍스트: app spdk_thread.
 *
 * 호출 체인:
 *   bdev_nvme_reset_ctrlr_complete / failover 경로 → [nvme_ctrlr_disconnect]
 *     → spdk_nvme_ctrlr_disconnect → (완료 후) disconnected_cb
 */
static void
nvme_ctrlr_disconnect(struct nvme_ctrlr *nvme_ctrlr, nvme_ctrlr_disconnected_cb cb_fn)
{
	int rc;                                         /* [한국어] disconnect 개시 반환 코드. */

	NVME_CTRLR_INFOLOG(nvme_ctrlr, "Start disconnecting ctrlr.\n");

	rc = spdk_nvme_ctrlr_disconnect(nvme_ctrlr->ctrlr); /* [한국어] 드라이버에 연결 해제 요청(비동기 시작). */
	if (rc != 0) {                                  /* [한국어] 이미 resetting/removed면 실패. */
		NVME_CTRLR_WARNLOG(nvme_ctrlr, "disconnecting ctrlr failed.\n");

		/* Disconnect fails if ctrlr is already resetting or removed. In this case,
		 * fail the reset sequence immediately.
		 */
		/* [한국어] (위 영어) 더 진행할 수 없으므로 reset 시퀀스를 즉시 실패로 마감. */
		bdev_nvme_reset_ctrlr_complete(nvme_ctrlr, false);
		return;
	}

	/* spdk_nvme_ctrlr_disconnect() may complete asynchronously later by polling adminq.
	 * Set callback here to execute the specified operation after ctrlr is really disconnected.
	 */
	/* [한국어] (위 영어) disconnect는 adminq 폴링으로 나중에 완료된다. 완료 시 수행할 콜백을 지금 등록. */
	assert(nvme_ctrlr->disconnected_cb == NULL);    /* [한국어] 이전 콜백이 남아있지 않아야 함(단일 진행). */
	nvme_ctrlr->disconnected_cb = cb_fn;            /* [한국어] adminq 폴러가 끊김을 감지하면 이 콜백 호출. */

	/* During disconnection, reduce the period to poll adminq more often. */
	/* [한국어] 끊김을 더 빨리 감지하기 위해 adminq 폴링 주기를 0으로(가능한 한 자주) 줄임. */
	bdev_nvme_change_adminq_poll_period(nvme_ctrlr, 0);
}

/* [한국어] reset 완료 후 컨트롤러에 대해 수행할 후속 동작 종류. */
enum bdev_nvme_op_after_reset {
	OP_NONE,                                        /* [한국어] 추가 동작 없음(정상 종료). */
	OP_COMPLETE_PENDING_DESTRUCT,                   /* [한국어] 대기 중이던 해제(destruct)를 이제 마무리. */
	OP_DESTRUCT,                                    /* [한국어] 손실 타임아웃 초과 → 컨트롤러 영구 삭제. */
	OP_DELAYED_RECONNECT,                           /* [한국어] reconnect_delay_sec 후 재연결 예약. */
	OP_FAILOVER,                                    /* [한국어] 대기 중이던 failover를 즉시 수행. */
};

/* [한국어] enum을 함수 반환 타입으로 쓰기 위한 typedef 별칭. */
typedef enum bdev_nvme_op_after_reset _bdev_nvme_op_after_reset;

/*
 * [한국어]
 * bdev_nvme_check_op_after_reset - reset 결과/상태를 보고 다음에 취할 동작을 결정한다.
 *
 * @nvme_ctrlr: reset이 막 끝난 컨트롤러.
 * @success: reset 성공 여부.
 * @pending_failover: reset 진행 중에 failover 요청이 쌓였는지(경쟁 조건 보정용).
 * @return: OP_* 열거값 — 호출자(reset 완료 핸들러)가 이 값에 따라 분기한다.
 *
 * 왜 필요한가: reset 완료 시점의 상태(삭제 대기/성공/타임아웃/실패)에 따라 후속 동작이 갈린다.
 * 이 함수가 정책 결정을 한곳에 모아 호출자의 switch를 단순화한다. 동작: (1) 컨트롤러가 등록 해제
 * 가능 상태면 OP_COMPLETE_PENDING_DESTRUCT, (2) 성공이거나 reconnect_delay=0이면 pending_failover
 * 유무에 따라 OP_FAILOVER/OP_NONE, (3) 손실 타임아웃 초과면 OP_DESTRUCT, (4) 그 외엔 OP_DELAYED_RECONNECT.
 * 실행 컨텍스트: app spdk_thread, mutex 보유 상태.
 *
 * 호출 체인:
 *   bdev_nvme_reset_ctrlr_complete → [bdev_nvme_check_op_after_reset]
 */
static _bdev_nvme_op_after_reset
bdev_nvme_check_op_after_reset(struct nvme_ctrlr *nvme_ctrlr, bool success,
			       bool pending_failover)
{
	if (nvme_ctrlr_can_be_unregistered(nvme_ctrlr)) { /* [한국어] 삭제 대기 + 참조 0이면. */
		/* Complete pending destruct after reset completes. */
		return OP_COMPLETE_PENDING_DESTRUCT;    /* [한국어] reset 끝났으니 미뤄둔 해제를 마무리. */
	} else if (success || nvme_ctrlr->opts.reconnect_delay_sec == 0) { /* [한국어] 성공 또는 즉시 재시도 정책. */
		if (pending_failover) {
			/* This is a fix for a race condition that failover was lost
			 * if fabric connect command got timeout while ctrlr was being
			 * reset and reset succeeded. We check connection establishment
			 * sequentially now but any network error can happen during reset.
			 * We have to keep this fix.
			 *
			 * On the other hand, if reset failed, delayed reconnect will be
			 * executed. In this case, we do not have to failover immediately.
			 */
			/* [한국어] (위 영어) reset 중 failover 요청이 유실되는 경쟁 조건 보정.
			 * reset이 성공했는데 failover가 대기 중이면 지금 즉시 failover 수행. */
			return OP_FAILOVER;
		} else {
			return OP_NONE;                 /* [한국어] 대기 failover 없으면 추가 동작 없음. */
		}
	} else if (bdev_nvme_check_ctrlr_loss_timeout(nvme_ctrlr)) { /* [한국어] 실패 + 손실 타임아웃 초과. */
		return OP_DESTRUCT;                     /* [한국어] 포기하고 컨트롤러 영구 삭제. */
	} else {
		return OP_DELAYED_RECONNECT;            /* [한국어] 실패 but 유예 내 → 지연 후 재연결. */
	}
}

/* [한국어] 전방 선언: 컨트롤러를 삭제(hotplug 여부 인자)하는 함수. */
static int bdev_nvme_delete_ctrlr(struct nvme_ctrlr *nvme_ctrlr, bool hotplug);
/* [한국어] 전방 선언: 컨트롤러 재연결 시퀀스를 시작하는 함수. */
static void bdev_nvme_reconnect_ctrlr(struct nvme_ctrlr *nvme_ctrlr);

/*
 * [한국어]
 * bdev_nvme_reconnect_delay_timer_expired - reconnect 지연 타이머 만료 시 재연결을 개시하는 폴러 콜백.
 *
 * @ctx: nvme_ctrlr 포인터(타이머 등록 시 전달).
 * @return: SPDK_POLLER_BUSY (이 호출에서 작업을 했음을 알림 — 일회성 타이머라 곧 해제됨).
 *
 * 왜 필요한가: 연결 실패 후 reconnect_delay_sec 만큼 backoff한 뒤 자동으로 재연결을 시도하기 위한
 * 일회성 타이머 폴러. 동작: (1) 타이머 자신을 unregister, (2) reconnect_is_delayed 플래그가 꺼졌으면
 * (취소됨) 그냥 종료, (3) destruct 중이면 종료, (4) resetting=true로 표시 후 adminq 폴러를 재개하고
 * bdev_nvme_reconnect_ctrlr로 실제 재연결 시작.
 * 실행 컨텍스트: 컨트롤러가 속한 app spdk_thread의 타이머 폴러. mutex로 상태 플래그 보호.
 *
 * 호출 체인:
 *   SPDK 타이머 폴러 런타임 → [bdev_nvme_reconnect_delay_timer_expired] → bdev_nvme_reconnect_ctrlr
 */
static int
bdev_nvme_reconnect_delay_timer_expired(void *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;            /* [한국어] 타이머가 묶인 컨트롤러. */

	SPDK_DTRACE_PROBE1(bdev_nvme_ctrlr_reconnect_delay, nvme_ctrlr->nbdev_ctrlr->name); /* [한국어] 재연결 지연 만료 추적. */
	pthread_mutex_lock(&nvme_ctrlr->mutex);         /* [한국어] reconnect/destruct 플래그를 원자적으로 검사·변경. */

	spdk_poller_unregister(&nvme_ctrlr->reconnect_delay_timer); /* [한국어] 일회성 타이머이므로 즉시 해제. */

	if (!nvme_ctrlr->reconnect_is_delayed) {        /* [한국어] 그사이 재연결 예약이 취소됐으면. */
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return SPDK_POLLER_BUSY;                /* [한국어] 아무 동작 없이 종료. */
	}

	nvme_ctrlr->reconnect_is_delayed = false;       /* [한국어] 지연 상태 해제(이제 실제 재연결로 진행). */

	if (nvme_ctrlr->destruct) {                     /* [한국어] 삭제 진행 중이면 재연결하지 않음. */
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return SPDK_POLLER_BUSY;
	}

	assert(nvme_ctrlr->resetting == false);         /* [한국어] 재연결 직전엔 reset 진행 중이 아니어야 함. */
	nvme_ctrlr->resetting = true;                   /* [한국어] reset/reconnect 진행 표시(중복 진입 차단). */

	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	spdk_poller_resume(nvme_ctrlr->adminq_timer_poller); /* [한국어] 일시정지했던 adminq 폴러 재개(연결 진행 폴링). */

	bdev_nvme_reconnect_ctrlr(nvme_ctrlr);          /* [한국어] 실제 재연결 시퀀스 개시. */
	return SPDK_POLLER_BUSY;                        /* [한국어] 작업 수행했음을 폴러 런타임에 보고. */
}

/*
 * [한국어]
 * bdev_nvme_start_reconnect_delay_timer - reconnect_delay_sec 후 재연결하도록 지연 타이머를 건다.
 *
 * @nvme_ctrlr: 재연결을 미룰 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 연결이 끊긴 직후 곧바로 재시도하면 죽은 타깃에 폭주가 발생하므로, 일정 시간(backoff)
 * 뒤에 한 번 재연결하도록 일회성 타이머를 등록한다. 동작: (1) adminq 폴러를 일시정지(끊긴 상태에서
 * 불필요한 폴링 방지), (2) reconnect_is_delayed=true 표시, (3) reconnect_delay_sec(초)를 마이크로초로
 * 환산해 타이머 폴러 등록.
 * 실행 컨텍스트: app spdk_thread. nvme_ctrlr_disconnect의 disconnected_cb로 호출되는 경우가 많다.
 *
 * 호출 체인:
 *   bdev_nvme_reset_ctrlr_complete(OP_DELAYED_RECONNECT) → nvme_ctrlr_disconnect(cb=이 함수)
 *     → [bdev_nvme_start_reconnect_delay_timer]
 */
static void
bdev_nvme_start_reconnect_delay_timer(struct nvme_ctrlr *nvme_ctrlr)
{
	spdk_poller_pause(nvme_ctrlr->adminq_timer_poller); /* [한국어] 끊긴 동안 adminq 폴링 일시정지(자원 절약). */

	assert(nvme_ctrlr->reconnect_is_delayed == false); /* [한국어] 중복 지연 등록 방지. */
	nvme_ctrlr->reconnect_is_delayed = true;        /* [한국어] 재연결이 지연 예약됨을 표시. */

	assert(nvme_ctrlr->reconnect_delay_timer == NULL); /* [한국어] 기존 타이머가 없어야 함. */
	/* [한국어] reconnect_delay_sec(초) → 마이크로초 환산 후 일회성 타이머 등록. */
	nvme_ctrlr->reconnect_delay_timer = SPDK_POLLER_REGISTER(bdev_nvme_reconnect_delay_timer_expired,
					    nvme_ctrlr,
					    nvme_ctrlr->opts.reconnect_delay_sec * SPDK_SEC_TO_USEC);
}

/* [한국어] 전방 선언: discovery 서비스 항목에서 이 컨트롤러를 제거하는 함수. */
static void remove_discovery_entry(struct nvme_ctrlr *nvme_ctrlr);

/*
 * [한국어]
 * bdev_nvme_reset_ctrlr_complete - 컨트롤러 리셋/페일오버 완료 후 상태를 정리하는 내부 함수.
 *
 * @nvme_ctrlr: 완료된 리셋 대상 컨트롤러
 * @success: 리셋/재연결 성공 여부
 * @return: void
 *
 * 리셋 완료 시 호출되며, 다음 작업을 결정한다:
 *   1) 리셋 실패 시 다음 alternate trid로 즉시 페일오버 또는 reconnect_delay_sec 후 재시도.
 *   2) pending_resets 완료, resetting/in_failover 플래그 클리어.
 *   3) op_after_reset 상태에 따라 남은 destruct/failover/reconnect 시퀀스 실행.
 * 컨텍스트: app 스레드 강제. mutex를 획득 후 상태를 조작한다.
 *
 * 호출 체인:
 *   bdev_nvme_reset_create_qpairs_done() → [이 함수] → nvme_ctrlr_disconnect / remove_discovery_entry 등
 */
static void
bdev_nvme_reset_ctrlr_complete(struct nvme_ctrlr *nvme_ctrlr, bool success)
{
	bdev_nvme_ctrlr_op_cb ctrlr_op_cb_fn = nvme_ctrlr->ctrlr_op_cb_fn;
	void *ctrlr_op_cb_arg = nvme_ctrlr->ctrlr_op_cb_arg;
	bool pending_failover;
	enum bdev_nvme_op_after_reset op_after_reset;

	assert(spdk_thread_is_app_thread(NULL));

	pthread_mutex_lock(&nvme_ctrlr->mutex);

	pending_failover = nvme_ctrlr->pending_failover;
	nvme_ctrlr->pending_failover = false;

	if (!success) {
		/* Connecting the active trid failed. Set the next alternate trid to the
		 * active trid if it exists.
		 */
		if (bdev_nvme_failover_trid(nvme_ctrlr, false, false)) {
			/* The next alternate trid exists and is ready to try. Try it now. */
			pthread_mutex_unlock(&nvme_ctrlr->mutex);

			NVME_CTRLR_INFOLOG(nvme_ctrlr, "Try the next alternate trid now.\n");
			nvme_ctrlr_disconnect(nvme_ctrlr, bdev_nvme_reconnect_ctrlr);
			return;
		}

		/* We came here if there is no alternate trid or if the next trid exists but
		 * is not ready to try. We will try the active trid after reconnect_delay_sec
		 * seconds if it is non-zero or at the next reset call otherwise.
		 */
	} else {
		/* Connecting the active trid succeeded. Clear the last failed time because it
		 * means the trid is failed if its last failed time is non-zero.
		 */
		nvme_ctrlr->active_path_id->last_failed_tsc = 0;
	}

	NVME_CTRLR_INFOLOG(nvme_ctrlr, "Clear pending resets.\n");

	/* Make sure we clear any pending resets before returning. */
	bdev_nvme_complete_pending_resets(nvme_ctrlr, success);

	if (!success) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "Resetting controller failed.\n");
		if (bdev_nvme_check_fast_io_fail_timeout(nvme_ctrlr)) {
			nvme_ctrlr->fast_io_fail_timedout = true;
		}
	} else {
		NVME_CTRLR_NOTICELOG(nvme_ctrlr, "Resetting controller successful.\n");
		nvme_ctrlr->reset_start_tsc = 0;
	}

	nvme_ctrlr->resetting = false;
	nvme_ctrlr->dont_retry = false;
	nvme_ctrlr->in_failover = false;

	nvme_ctrlr->ctrlr_op_cb_fn = NULL;
	nvme_ctrlr->ctrlr_op_cb_arg = NULL;

	op_after_reset = bdev_nvme_check_op_after_reset(nvme_ctrlr, success, pending_failover);
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	/* Delay callbacks when the next operation is a failover. */
	if (ctrlr_op_cb_fn && op_after_reset != OP_FAILOVER) {
		ctrlr_op_cb_fn(ctrlr_op_cb_arg, success ? 0 : -1);
	}

	switch (op_after_reset) {
	case OP_COMPLETE_PENDING_DESTRUCT:
		nvme_ctrlr_unregister(nvme_ctrlr);
		break;
	case OP_DESTRUCT:
		bdev_nvme_delete_ctrlr(nvme_ctrlr, false);
		remove_discovery_entry(nvme_ctrlr);
		break;
	case OP_DELAYED_RECONNECT:
		nvme_ctrlr_disconnect(nvme_ctrlr, bdev_nvme_start_reconnect_delay_timer);
		break;
	case OP_FAILOVER:
		nvme_ctrlr->ctrlr_op_cb_fn = ctrlr_op_cb_fn;
		nvme_ctrlr->ctrlr_op_cb_arg = ctrlr_op_cb_arg;
		bdev_nvme_failover_ctrlr(nvme_ctrlr);
		break;
	default:
		break;
	}
}

/*
 * [한국어]
 * bdev_nvme_reset_create_qpairs_failed - qpair 생성 실패 시 리셋 시퀀스를 실패로 완료.
 *
 * @nvme_ctrlr: 대상 컨트롤러
 * @ctx: 사용하지 않음
 * @status: 에러 코드
 * @return: void
 *
 * nvme_ctrlr_for_each_channel 완료 콜백. qpair 생성에 실패한 경우
 * bdev_nvme_reset_ctrlr_complete(success=false)를 호출해 리셋 실패로 처리한다.
 *
 * 호출 체인:
 *   bdev_nvme_reset_create_qpairs_done() → nvme_ctrlr_for_each_channel → [이 함수]
 *   → bdev_nvme_reset_ctrlr_complete(false)
 */
static void
bdev_nvme_reset_create_qpairs_failed(struct nvme_ctrlr *nvme_ctrlr, void *ctx, int status)
{
	bdev_nvme_reset_ctrlr_complete(nvme_ctrlr, false);
}

/*
 * [한국어]
 * bdev_nvme_reset_destroy_qpair - 리셋 시퀀스에서 각 채널의 qpair를 비동기 연결 해제.
 *
 * @i: 채널 이터레이터
 * @nvme_ctrlr: 대상 컨트롤러
 * @ctrlr_ch: 현재 처리 중인 컨트롤러 채널
 * @ctx: 사용하지 않음
 * @return: void
 *
 * nvme_ctrlr_for_each_channel의 채널별 콜백. 각 채널의 io_path 캐시를 클리어하고
 * qpair를 disconnect한다. qpair가 실제로 disconnected될 때까지는 reset_iter를 보관하고
 * bdev_nvme_disconnected_qpair_cb에서 nvme_ctrlr_for_each_channel_continue를 호출한다.
 *
 * 호출 체인:
 *   nvme_ctrlr_for_each_channel() → [이 함수] → spdk_nvme_ctrlr_disconnect_io_qpair()
 *   → (비동기) bdev_nvme_disconnected_qpair_cb → nvme_ctrlr_channel_reset_finish
 */
static void
bdev_nvme_reset_destroy_qpair(struct nvme_ctrlr_channel_iter *i,
			      struct nvme_ctrlr *nvme_ctrlr,
			      struct nvme_ctrlr_channel *ctrlr_ch, void *ctx)
{
	struct nvme_qpair *nvme_qpair;
	struct spdk_nvme_qpair *qpair;

	nvme_qpair = ctrlr_ch->qpair;
	assert(nvme_qpair != NULL);

	_bdev_nvme_clear_io_path_cache(nvme_qpair);

	qpair = nvme_qpair->qpair;
	if (qpair != NULL) {
		NVME_QPAIR_INFOLOG(nvme_qpair, "Start disconnecting qpair.\n");

		if (nvme_qpair->ctrlr->dont_retry) {
			spdk_nvme_qpair_set_abort_dnr(qpair, true);
		}
		spdk_nvme_ctrlr_disconnect_io_qpair(qpair);

		/* The current full reset sequence will move to the next
		 * ctrlr_channel after the qpair is actually disconnected.
		 */
		assert(ctrlr_ch->reset_iter == NULL);
		ctrlr_ch->reset_iter = i;
	} else {
		nvme_ctrlr_for_each_channel_continue(i, 0);
	}
}

/*
 * [한국어]
 * bdev_nvme_reset_create_qpairs_done - 리셋 후 모든 qpair 생성 완료 콜백.
 *
 * @nvme_ctrlr: 대상 컨트롤러
 * @ctx: 사용하지 않음
 * @status: 0이면 전체 qpair 생성 성공, 비0이면 하나 이상 실패
 * @return: void
 *
 * nvme_ctrlr_for_each_channel의 완료 콜백.
 * 성공 시 bdev_nvme_reset_ctrlr_complete(true)로 리셋 완료를 알린다.
 * 실패 시 생성된 qpair들을 정리(bdev_nvme_reset_destroy_qpair)하고
 * bdev_nvme_reset_create_qpairs_failed에서 리셋 실패로 처리한다.
 *
 * 호출 체인:
 *   nvme_ctrlr_for_each_channel(bdev_nvme_reset_create_qpair) → [이 함수]
 *   → bdev_nvme_reset_ctrlr_complete(true) 또는
 *   → nvme_ctrlr_for_each_channel(bdev_nvme_reset_destroy_qpair)
 */
static void
bdev_nvme_reset_create_qpairs_done(struct nvme_ctrlr *nvme_ctrlr, void *ctx, int status)
{
	if (status == 0) {
		NVME_CTRLR_INFOLOG(nvme_ctrlr, "qpairs were created after ctrlr reset.\n");

		bdev_nvme_reset_ctrlr_complete(nvme_ctrlr, true);
	} else {
		NVME_CTRLR_INFOLOG(nvme_ctrlr, "qpairs were failed to create after ctrlr reset.\n");

		/* Delete the added qpairs and quiesce ctrlr to make the states clean. */
		nvme_ctrlr_for_each_channel(nvme_ctrlr,
					    bdev_nvme_reset_destroy_qpair,
					    NULL,
					    bdev_nvme_reset_create_qpairs_failed);
	}
}

/*
 * [한국어]
 * bdev_nvme_reset_check_qpair_connected - 리셋 중 qpair 연결 완료 여부를 폴링하는 poller.
 *
 * @ctx: nvme_ctrlr_channel 포인터 (connect_poller에 등록된 인자)
 * @return: SPDK_POLLER_BUSY (연결 중) 또는 SPDK_POLLER_BUSY (완료 후도 마찬가지)
 *
 * bdev_nvme_reset_create_qpair()에서 SPDK_POLLER_REGISTER로 등록.
 * spdk_nvme_qpair_is_connected()가 true가 되면 nvme_ctrlr_channel_reset_finish()로
 * 채널별 리셋 단계를 완료하고, 자동 페일백이 활성이면 io_path 캐시를 클리어한다.
 * reset_iter가 NULL이면 qpair 연결이 이미 실패한 상태이므로 abort 중이다.
 *
 * 호출 체인:
 *   SPDK_POLLER_REGISTER → [이 함수] → nvme_ctrlr_channel_reset_finish()
 *   → nvme_ctrlr_for_each_channel_continue()
 */
static int
bdev_nvme_reset_check_qpair_connected(void *ctx)
{
	struct nvme_ctrlr_channel *ctrlr_ch = ctx;
	struct nvme_qpair *nvme_qpair = ctrlr_ch->qpair;
	struct spdk_nvme_qpair *qpair;

	if (ctrlr_ch->reset_iter == NULL) {
		/* qpair was already failed to connect and the reset sequence is being aborted. */
		assert(ctrlr_ch->connect_poller == NULL);
		assert(nvme_qpair->qpair == NULL);

		NVME_CTRLR_INFOLOG(nvme_qpair->ctrlr,
				   "qpair was already failed to connect. reset is being aborted.\n");
		return SPDK_POLLER_BUSY;
	}

	qpair = nvme_qpair->qpair;
	assert(qpair != NULL);

	if (!spdk_nvme_qpair_is_connected(qpair)) {
		return SPDK_POLLER_BUSY;
	}

	NVME_QPAIR_INFOLOG(nvme_qpair, "qpair was connected.\n");
	nvme_ctrlr_channel_reset_finish(ctrlr_ch, 0);

	if (!g_opts.disable_auto_failback) {
		_bdev_nvme_clear_io_path_cache(nvme_qpair);
	}

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * bdev_nvme_reset_create_qpair - 리셋 후 각 채널의 qpair를 새로 생성하고 연결 폴링 시작.
 *
 * @i: 채널 이터레이터 (for_each_channel 프레임워크)
 * @nvme_ctrlr: 대상 컨트롤러
 * @ctrlr_ch: 현재 처리 중인 컨트롤러 채널
 * @ctx: 사용하지 않음
 * @return: void
 *
 * 리셋 완료 후 nvme_ctrlr_for_each_channel 루프에서 각 채널마다 호출됨.
 * bdev_nvme_create_qpair()로 새 qpair를 생성하고, 성공 시
 * bdev_nvme_reset_check_qpair_connected poller를 등록해 연결 완료를 기다린다.
 * 실패 시 reset_iter를 클리어하고 채널 루프를 에러로 계속 진행한다.
 *
 * 호출 체인:
 *   nvme_ctrlr_for_each_channel(bdev_nvme_reconnect_ctrlr_poll) → [이 함수]
 *   → bdev_nvme_create_qpair() → SPDK_POLLER_REGISTER(bdev_nvme_reset_check_qpair_connected)
 */
static void
bdev_nvme_reset_create_qpair(struct nvme_ctrlr_channel_iter *i,
			     struct nvme_ctrlr *nvme_ctrlr,
			     struct nvme_ctrlr_channel *ctrlr_ch,
			     void *ctx)
{
	struct nvme_qpair *nvme_qpair = ctrlr_ch->qpair;
	int rc = 0;

	if (nvme_qpair->qpair == NULL) {
		rc = bdev_nvme_create_qpair(nvme_qpair);
	}
	if (rc == 0) {
		ctrlr_ch->connect_poller = SPDK_POLLER_REGISTER(bdev_nvme_reset_check_qpair_connected,
					   ctrlr_ch, 0);

		NVME_QPAIR_INFOLOG(nvme_qpair, "Start checking qpair to be connected.\n");

		/* The current full reset sequence will move to the next
		 * ctrlr_channel after the qpair is actually connected.
		 */
		assert(ctrlr_ch->reset_iter == NULL);
		ctrlr_ch->reset_iter = i;
	} else {
		nvme_ctrlr_for_each_channel_continue(i, rc);
	}
}

/*
 * [한국어]
 * nvme_ctrlr_check_namespaces - 리셋 후 제거된 namespace를 감지하여 ns 포인터를 NULL로 설정.
 *
 * @nvme_ctrlr: 대상 컨트롤러
 * @return: void
 *
 * 컨트롤러 재연결 성공 후(bdev_nvme_reconnect_ctrlr_poll에서) 호출.
 * 리셋 중에 remove된 namespace는 spdk_nvme_ctrlr_is_active_ns()가 false를 반환하므로
 * nvme_ns->ns를 NULL로 설정한다. 이후 nvme_ctrlr_populate_namespaces()에서
 * 실제 detach/depopulate 처리가 이루어진다.
 * 컨텍스트: app 스레드 강제.
 *
 * 호출 체인:
 *   bdev_nvme_reconnect_ctrlr_poll() → [이 함수] → nvme_ctrlr_for_each_channel(bdev_nvme_reset_create_qpair)
 */
static void
nvme_ctrlr_check_namespaces(struct nvme_ctrlr *nvme_ctrlr)
{
	struct spdk_nvme_ctrlr *ctrlr = nvme_ctrlr->ctrlr;
	struct nvme_ns *nvme_ns;

	assert(spdk_thread_is_app_thread(NULL));

	RB_FOREACH(nvme_ns, nvme_ns_tree, &nvme_ctrlr->namespaces) {
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr, nvme_ns->id)) {
			NVME_NS_DEBUGLOG(nvme_ns, "NSID was removed during reset.\n");
			/* NS can be added again. Just nullify nvme_ns->ns. */
			nvme_ns->ns = NULL;
		}
	}
}


/*
 * [한국어]
 * bdev_nvme_reconnect_ctrlr_poll - 컨트롤러 재연결 완료 여부를 폴링하는 poller.
 *
 * @arg: nvme_ctrlr 포인터 (reset_detach_poller에 등록된 인자)
 * @return: SPDK_POLLER_BUSY (재연결 중) 또는 SPDK_POLLER_BUSY (완료 후 처리)
 *
 * bdev_nvme_reconnect_ctrlr()에서 reset_detach_poller로 등록됨.
 * spdk_nvme_ctrlr_reconnect_poll_async()를 반복 호출해 연결 완료를 감지.
 * ctrlr_loss_timeout 초과 시 스스로 fail 처리.
 * 연결 성공 시 namespace 확인 후 qpair 재생성 시퀀스 시작.
 * 연결 실패 시 bdev_nvme_reset_ctrlr_complete(false)로 리셋 실패 처리.
 *
 * 호출 체인:
 *   SPDK_POLLER_REGISTER → [이 함수] → 성공: nvme_ctrlr_for_each_channel(bdev_nvme_reset_create_qpair)
 *                                   → 실패: bdev_nvme_reset_ctrlr_complete(false)
 */
static int
bdev_nvme_reconnect_ctrlr_poll(void *arg)
{
	struct nvme_ctrlr *nvme_ctrlr = arg;
	int rc = -ETIMEDOUT;

	if (bdev_nvme_check_ctrlr_loss_timeout(nvme_ctrlr)) {
		/* Mark the ctrlr as failed. The next call to
		 * spdk_nvme_ctrlr_reconnect_poll_async() will then
		 * do the necessary cleanup and return failure.
		 */
		spdk_nvme_ctrlr_fail(nvme_ctrlr->ctrlr);
	}

	rc = spdk_nvme_ctrlr_reconnect_poll_async(nvme_ctrlr->ctrlr);
	if (rc == -EAGAIN) {
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&nvme_ctrlr->reset_detach_poller);
	if (rc == 0) {
		NVME_CTRLR_INFOLOG(nvme_ctrlr, "ctrlr was connected. Create qpairs.\n");
		nvme_ctrlr_check_namespaces(nvme_ctrlr);

		/* Recreate all of the I/O queue pairs */
		nvme_ctrlr_for_each_channel(nvme_ctrlr,
					    bdev_nvme_reset_create_qpair,
					    NULL,
					    bdev_nvme_reset_create_qpairs_done);
	} else {
		NVME_CTRLR_INFOLOG(nvme_ctrlr, "ctrlr could not be connected.\n");

		bdev_nvme_reset_ctrlr_complete(nvme_ctrlr, false);
	}
	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * bdev_nvme_reconnect_ctrlr - 컨트롤러 비동기 재연결을 시작하고 poll poller를 등록.
 *
 * @nvme_ctrlr: 재연결할 컨트롤러
 * @return: void
 *
 * spdk_nvme_ctrlr_reconnect_async()로 lib/nvme 수준의 비동기 재연결을 개시하고
 * reset_detach_poller에 bdev_nvme_reconnect_ctrlr_poll을 등록해 완료를 대기한다.
 * dtrace probe: bdev_nvme_ctrlr_reconnect.
 * 컨텍스트: app 스레드(spdk_thread_send_msg 또는 reconnect_delay_timer 콜백).
 *
 * 호출 체인:
 *   bdev_nvme_reset_destroy_qpair_done() → [이 함수]
 *   또는 nvme_ctrlr_disconnect(callback=bdev_nvme_reconnect_ctrlr) → [이 함수]
 *   → SPDK_POLLER_REGISTER(bdev_nvme_reconnect_ctrlr_poll)
 */
static void
bdev_nvme_reconnect_ctrlr(struct nvme_ctrlr *nvme_ctrlr)
{
	NVME_CTRLR_INFOLOG(nvme_ctrlr, "Start reconnecting ctrlr.\n");

	spdk_nvme_ctrlr_reconnect_async(nvme_ctrlr->ctrlr);

	SPDK_DTRACE_PROBE1(bdev_nvme_ctrlr_reconnect, nvme_ctrlr->nbdev_ctrlr->name);
	assert(nvme_ctrlr->reset_detach_poller == NULL);
	nvme_ctrlr->reset_detach_poller = SPDK_POLLER_REGISTER(bdev_nvme_reconnect_ctrlr_poll,
					  nvme_ctrlr, 0);
}

/*
 * [한국어]
 * bdev_nvme_reset_destroy_qpair_done - qpair 전부 해제 완료 후 재연결 단계로 진입.
 *
 * @nvme_ctrlr: 대상 컨트롤러
 * @ctx: 사용하지 않음
 * @status: 0 고정 (assert로 검증)
 * @return: void
 *
 * nvme_ctrlr_for_each_channel(bdev_nvme_reset_destroy_qpair)의 완료 콜백.
 * Fabrics transport인 경우 nvme_ctrlr_disconnect 후 bdev_nvme_reconnect_ctrlr를 호출.
 * PCIe인 경우 disconnect 없이 bdev_nvme_reconnect_ctrlr를 직접 호출.
 * dtrace probe: bdev_nvme_ctrlr_reset.
 *
 * 호출 체인:
 *   nvme_ctrlr_for_each_channel(bdev_nvme_reset_destroy_qpair) → [이 함수]
 *   → nvme_ctrlr_disconnect() 또는 bdev_nvme_reconnect_ctrlr()
 */
static void
bdev_nvme_reset_destroy_qpair_done(struct nvme_ctrlr *nvme_ctrlr, void *ctx, int status)
{
	SPDK_DTRACE_PROBE1(bdev_nvme_ctrlr_reset, nvme_ctrlr->nbdev_ctrlr->name);
	assert(status == 0);

	NVME_CTRLR_INFOLOG(nvme_ctrlr, "qpairs were deleted.\n");

	if (!spdk_nvme_ctrlr_is_fabrics(nvme_ctrlr->ctrlr)) {
		bdev_nvme_reconnect_ctrlr(nvme_ctrlr);
	} else {
		nvme_ctrlr_disconnect(nvme_ctrlr, bdev_nvme_reconnect_ctrlr);
	}
}

/*
 * [한국어]
 * bdev_nvme_reset_destroy_qpairs - 리셋 시퀀스 시작: 모든 채널의 qpair 해제 루프 시작.
 *
 * @nvme_ctrlr: 대상 컨트롤러
 * @return: void
 *
 * _bdev_nvme_reset_ctrlr에서 호출. nvme_ctrlr_for_each_channel을 통해
 * 모든 스레드의 ctrlr_channel에 bdev_nvme_reset_destroy_qpair를 적용한다.
 * 완료 콜백은 bdev_nvme_reset_destroy_qpair_done.
 *
 * 호출 체인:
 *   _bdev_nvme_reset_ctrlr() → [이 함수]
 *   → nvme_ctrlr_for_each_channel(bdev_nvme_reset_destroy_qpair)
 *   → bdev_nvme_reset_destroy_qpair_done()
 */
static void
bdev_nvme_reset_destroy_qpairs(struct nvme_ctrlr *nvme_ctrlr)
{
	NVME_CTRLR_INFOLOG(nvme_ctrlr, "Delete qpairs for reset.\n");

	nvme_ctrlr_for_each_channel(nvme_ctrlr,
				    bdev_nvme_reset_destroy_qpair,
				    NULL,
				    bdev_nvme_reset_destroy_qpair_done);
}

/*
 * [한국어]
 * bdev_nvme_reconnect_ctrlr_now - reconnect_delay_timer 만료 후 즉시 재연결 시작.
 *
 * @ctx: nvme_ctrlr 포인터 (타이머 콜백 인자)
 * @return: void
 *
 * reconnect_delay_sec 후 reconnect_delay_timer 만료 시 호출.
 * 타이머를 해제하고 adminq_timer_poller를 재개(resume)한 뒤
 * bdev_nvme_reconnect_ctrlr()를 호출해 실제 재연결을 시작한다.
 * 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   SPDK_POLLER_REGISTER(reconnect_delay_timer) → [이 함수]
 *   → bdev_nvme_reconnect_ctrlr()
 */
static void
bdev_nvme_reconnect_ctrlr_now(void *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	assert(nvme_ctrlr->resetting == true);
	assert(spdk_thread_is_app_thread(NULL));

	spdk_poller_unregister(&nvme_ctrlr->reconnect_delay_timer);

	spdk_poller_resume(nvme_ctrlr->adminq_timer_poller);

	bdev_nvme_reconnect_ctrlr(nvme_ctrlr);
}

/*
 * [한국어]
 * _bdev_nvme_reset_ctrlr - app 스레드에서 실제 리셋 시퀀스를 시작하는 내부 함수.
 *
 * @ctx: nvme_ctrlr 포인터 (spdk_thread_send_msg의 메시지 인자)
 * @return: void
 *
 * bdev_nvme_reset_ctrlr_unsafe() 또는 bdev_nvme_failover_ctrlr()에서
 * spdk_thread_send_msg(app_thread, _bdev_nvme_reset_ctrlr)로 디스패치된다.
 * adminq_timer_poller를 일시 중지(pause)하고 bdev_nvme_reset_destroy_qpairs()로
 * 전체 리셋 시퀀스(qpair 해제 → 재연결 → qpair 재생성)를 시작한다.
 * reconnect_delay_timer가 이미 등록된 경우(재연결 지연 중)에는 시퀀스를 즉시 시작하지 않고
 * 타이머가 만료되면 bdev_nvme_reconnect_ctrlr_now가 호출된다.
 * 컨텍스트: app 스레드 강제.
 *
 * 호출 체인:
 *   bdev_nvme_reset_ctrlr_unsafe() / bdev_nvme_failover_ctrlr_unsafe()
 *   → spdk_thread_send_msg(app_thread) → [이 함수] → bdev_nvme_reset_destroy_qpairs()
 */
static void
_bdev_nvme_reset_ctrlr(void *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	assert(nvme_ctrlr->resetting == true);
	assert(spdk_thread_is_app_thread(NULL));

	if (!spdk_nvme_ctrlr_is_fabrics(nvme_ctrlr->ctrlr)) {
		nvme_ctrlr_disconnect(nvme_ctrlr, bdev_nvme_reset_destroy_qpairs);
	} else {
		bdev_nvme_reset_destroy_qpairs(nvme_ctrlr);
	}
}

/*
 * [한국어]
 * bdev_nvme_reset_ctrlr_unsafe - mutex 보유 상태에서 리셋 플래그를 설정하고 실행 함수를 결정.
 *
 * @nvme_ctrlr: 대상 컨트롤러
 * @msg_fn: 실행할 리셋 함수를 반환하는 출력 포인터
 *          - reconnect 지연 중: bdev_nvme_reconnect_ctrlr_now
 *          - 그 외: _bdev_nvme_reset_ctrlr
 * @return: 0(성공), -ENXIO(destruct 중), -EBUSY(already resetting), -EALREADY(disabled)
 *
 * bdev_nvme_reset_ctrlr()에서 mutex 보호 하에 호출됨.
 * resetting=true, dont_retry=true 플래그를 설정하고,
 * reconnect_delay 중이면 delay를 취소하고 즉시 재연결 함수를 선택한다.
 *
 * 호출 체인:
 *   bdev_nvme_reset_ctrlr() → [이 함수] → (mutex 해제) → spdk_thread_send_msg(msg_fn)
 */
static int
bdev_nvme_reset_ctrlr_unsafe(struct nvme_ctrlr *nvme_ctrlr, spdk_msg_fn *msg_fn)
{
	if (nvme_ctrlr->destruct) {
		return -ENXIO;
	}

	if (nvme_ctrlr->resetting) {
		NVME_CTRLR_NOTICELOG(nvme_ctrlr, "Unable to perform reset, already in progress.\n");
		return -EBUSY;
	}

	if (nvme_ctrlr->disabled) {
		NVME_CTRLR_NOTICELOG(nvme_ctrlr, "Unable to perform reset. Controller is disabled.\n");
		return -EALREADY;
	}

	nvme_ctrlr->resetting = true;
	nvme_ctrlr->dont_retry = true;

	if (nvme_ctrlr->reconnect_is_delayed) {
		NVME_CTRLR_INFOLOG(nvme_ctrlr, "Reconnect is already scheduled.\n");
		*msg_fn = bdev_nvme_reconnect_ctrlr_now;
		nvme_ctrlr->reconnect_is_delayed = false;
	} else {
		*msg_fn = _bdev_nvme_reset_ctrlr;
	}

	if (nvme_ctrlr->reset_start_tsc == 0) {
		nvme_ctrlr->reset_start_tsc = spdk_get_ticks();
	}

	return 0;
}

/*
 * [한국어]
 * bdev_nvme_reset_ctrlr - 컨트롤러 리셋을 시작하는 외부 진입점(thread-safe).
 *
 * @nvme_ctrlr: 리셋할 컨트롤러
 * @return: 0(성공 시작), -ENXIO/-EBUSY/-EALREADY(에러)
 *
 * mutex를 획득해 bdev_nvme_reset_ctrlr_unsafe()로 상태 플래그를 설정한 후,
 * spdk_thread_send_msg(app_thread)로 리셋 시퀀스를 app 스레드에서 비동기 실행한다.
 * timeout_cb, failover, RPC 등에서 호출됨.
 * 컨텍스트: 어느 스레드에서도 호출 가능.
 *
 * 호출 체인:
 *   timeout_cb() / nvme_abort_cpl() / bdev_nvme_check_fast_io_fail_timeout() →
 *   [이 함수] → spdk_thread_send_msg → _bdev_nvme_reset_ctrlr / bdev_nvme_reconnect_ctrlr_now
 */
static int
bdev_nvme_reset_ctrlr(struct nvme_ctrlr *nvme_ctrlr)
{
	spdk_msg_fn msg_fn;
	int rc;

	pthread_mutex_lock(&nvme_ctrlr->mutex);
	rc = bdev_nvme_reset_ctrlr_unsafe(nvme_ctrlr, &msg_fn);
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	if (rc == 0) {
		spdk_thread_send_msg(spdk_thread_get_app_thread(), msg_fn, nvme_ctrlr);
	}

	return rc;
}

/*
 * [한국어]
 * bdev_nvme_enable_ctrlr - disabled 상태의 컨트롤러를 다시 활성화(재연결 시작).
 *
 * @nvme_ctrlr: 활성화할 컨트롤러
 * @return: 0(성공), -ENXIO(destruct 중), -EBUSY(resetting 중), -EALREADY(이미 enabled)
 *
 * nvme_ctrlr_op(NVME_CTRLR_OP_ENABLE) RPC에서 호출.
 * disabled=false, resetting=true로 설정 후 bdev_nvme_reconnect_ctrlr_now()를 직접 호출해
 * 즉각 재연결 시퀀스를 시작한다.
 * 컨텍스트: app 스레드 강제.
 *
 * 호출 체인:
 *   nvme_ctrlr_op() → [이 함수] → bdev_nvme_reconnect_ctrlr_now() → bdev_nvme_reconnect_ctrlr()
 */
static int
bdev_nvme_enable_ctrlr(struct nvme_ctrlr *nvme_ctrlr)
{
	assert(spdk_thread_is_app_thread(NULL));

	pthread_mutex_lock(&nvme_ctrlr->mutex);
	if (nvme_ctrlr->destruct) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return -ENXIO;
	}

	if (nvme_ctrlr->resetting) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return -EBUSY;
	}

	if (!nvme_ctrlr->disabled) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return -EALREADY;
	}

	nvme_ctrlr->disabled = false;
	nvme_ctrlr->resetting = true;

	nvme_ctrlr->reset_start_tsc = spdk_get_ticks();

	bdev_nvme_reconnect_ctrlr_now(nvme_ctrlr);
	pthread_mutex_unlock(&nvme_ctrlr->mutex);
	return 0;
}

/*
 * [한국어]
 * bdev_nvme_disable_ctrlr_complete - qpair 전부 해제 및 disconnect 완료 후 컨트롤러를 disabled 상태로 전환.
 *
 * @nvme_ctrlr: 비활성화 완료할 컨트롤러
 * @return: void
 *
 * bdev_nvme_disable_destroy_qpairs_done()에서 직접 호출되거나
 * nvme_ctrlr_disconnect(callback=bdev_nvme_disable_ctrlr_complete)를 통해 호출됨.
 * disabled=true, resetting=false로 설정하고 adminq_timer_poller를 일시 중지.
 * pending_resets를 성공(true)으로 완료하고 ctrlr_op_cb를 호출.
 * nvme_ctrlr 레퍼런스 카운트 감소.
 *
 * 호출 체인:
 *   bdev_nvme_disable_destroy_qpairs_done() → [이 함수] 또는
 *   nvme_ctrlr_disconnect(bdev_nvme_disable_ctrlr_complete) → [이 함수]
 */
static void
bdev_nvme_disable_ctrlr_complete(struct nvme_ctrlr *nvme_ctrlr)
{
	bdev_nvme_ctrlr_op_cb ctrlr_op_cb_fn = nvme_ctrlr->ctrlr_op_cb_fn;
	void *ctrlr_op_cb_arg = nvme_ctrlr->ctrlr_op_cb_arg;

	assert(spdk_thread_is_app_thread(NULL));

	nvme_ctrlr->ctrlr_op_cb_fn = NULL;
	nvme_ctrlr->ctrlr_op_cb_arg = NULL;

	pthread_mutex_lock(&nvme_ctrlr->mutex);

	nvme_ctrlr->resetting = false;
	nvme_ctrlr->dont_retry = false;
	nvme_ctrlr->pending_failover = false;

	nvme_ctrlr->disabled = true;
	spdk_poller_pause(nvme_ctrlr->adminq_timer_poller);

	/* Make sure we clear any pending resets before returning. */
	bdev_nvme_complete_pending_resets(nvme_ctrlr, true);

	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	if (ctrlr_op_cb_fn) {
		ctrlr_op_cb_fn(ctrlr_op_cb_arg, 0);
	}

	nvme_ctrlr_put_ref(nvme_ctrlr);
}

/*
 * [한국어]
 * bdev_nvme_disable_destroy_qpairs_done - disable 시 qpair 전부 해제 완료 콜백.
 *
 * @nvme_ctrlr: 대상 컨트롤러
 * @ctx: 사용하지 않음
 * @status: 0 고정 (assert로 검증)
 * @return: void
 *
 * nvme_ctrlr_for_each_channel(bdev_nvme_reset_destroy_qpair)의 완료 콜백.
 * Fabrics transport: nvme_ctrlr_disconnect 후 bdev_nvme_disable_ctrlr_complete 호출.
 * PCIe: bdev_nvme_disable_ctrlr_complete를 직접 호출.
 *
 * 호출 체인:
 *   nvme_ctrlr_for_each_channel(bdev_nvme_reset_destroy_qpair) → [이 함수]
 *   → bdev_nvme_disable_ctrlr_complete() 또는 nvme_ctrlr_disconnect()
 */
static void
bdev_nvme_disable_destroy_qpairs_done(struct nvme_ctrlr *nvme_ctrlr, void *ctx, int status)
{
	assert(status == 0);

	if (!spdk_nvme_ctrlr_is_fabrics(nvme_ctrlr->ctrlr)) {
		bdev_nvme_disable_ctrlr_complete(nvme_ctrlr);
	} else {
		nvme_ctrlr_disconnect(nvme_ctrlr, bdev_nvme_disable_ctrlr_complete);
	}
}

/*
 * [한국어]
 * bdev_nvme_disable_destroy_qpairs - disable 시퀀스: 모든 채널의 qpair 해제 루프 시작.
 *
 * @nvme_ctrlr: 대상 컨트롤러
 * @return: void
 *
 * _bdev_nvme_disconnect_and_disable_ctrlr에서 호출.
 * nvme_ctrlr_for_each_channel로 각 채널에 bdev_nvme_reset_destroy_qpair 적용.
 * 완료 콜백은 bdev_nvme_disable_destroy_qpairs_done.
 *
 * 호출 체인:
 *   _bdev_nvme_disconnect_and_disable_ctrlr() → [이 함수]
 *   → nvme_ctrlr_for_each_channel(bdev_nvme_reset_destroy_qpair)
 *   → bdev_nvme_disable_destroy_qpairs_done()
 */
static void
bdev_nvme_disable_destroy_qpairs(struct nvme_ctrlr *nvme_ctrlr)
{
	nvme_ctrlr_for_each_channel(nvme_ctrlr,
				    bdev_nvme_reset_destroy_qpair,
				    NULL,
				    bdev_nvme_disable_destroy_qpairs_done);
}

/*
 * [한국어]
 * _bdev_nvme_cancel_reconnect_and_disable_ctrlr - reconnect 지연 타이머를 취소하고 즉시 disable 완료.
 *
 * @ctx: nvme_ctrlr 포인터 (spdk_thread_send_msg 메시지 인자)
 * @return: void
 *
 * bdev_nvme_disable_ctrlr()에서 reconnect_is_delayed=true인 경우에
 * 이 함수가 msg_fn으로 선택됨. reconnect_delay_timer를 해제하고
 * bdev_nvme_disable_ctrlr_complete()를 직접 호출해 qpair 해제 없이 disable 완료.
 * 컨텍스트: app 스레드 강제.
 *
 * 호출 체인:
 *   bdev_nvme_disable_ctrlr() → spdk_thread_send_msg → [이 함수]
 *   → bdev_nvme_disable_ctrlr_complete()
 */
static void
_bdev_nvme_cancel_reconnect_and_disable_ctrlr(void *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	assert(nvme_ctrlr->resetting == true);
	assert(spdk_thread_is_app_thread(NULL));

	spdk_poller_unregister(&nvme_ctrlr->reconnect_delay_timer);

	bdev_nvme_disable_ctrlr_complete(nvme_ctrlr);
}

/*
 * [한국어]
 * _bdev_nvme_disconnect_and_disable_ctrlr - app 스레드에서 disable 시퀀스(qpair 해제 → disconnect)를 시작.
 *
 * @ctx: nvme_ctrlr 포인터 (spdk_thread_send_msg 메시지 인자)
 * @return: void
 *
 * bdev_nvme_disable_ctrlr()에서 reconnect_is_delayed=false인 경우에
 * 이 함수가 msg_fn으로 선택됨.
 * PCIe: 먼저 nvme_ctrlr_disconnect 후 bdev_nvme_disable_destroy_qpairs 실행.
 * Fabrics: bdev_nvme_disable_destroy_qpairs를 직접 실행.
 * 컨텍스트: app 스레드 강제.
 *
 * 호출 체인:
 *   bdev_nvme_disable_ctrlr() → spdk_thread_send_msg → [이 함수]
 *   → nvme_ctrlr_disconnect() / bdev_nvme_disable_destroy_qpairs()
 */
static void
_bdev_nvme_disconnect_and_disable_ctrlr(void *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	assert(nvme_ctrlr->resetting == true);
	assert(spdk_thread_is_app_thread(NULL));

	if (!spdk_nvme_ctrlr_is_fabrics(nvme_ctrlr->ctrlr)) {
		nvme_ctrlr_disconnect(nvme_ctrlr, bdev_nvme_disable_destroy_qpairs);
	} else {
		bdev_nvme_disable_destroy_qpairs(nvme_ctrlr);
	}
}

/*
 * [한국어]
 * bdev_nvme_disable_ctrlr - 컨트롤러를 비활성화하여 I/O를 차단하고 qpair를 해제.
 *
 * @nvme_ctrlr: 비활성화할 컨트롤러
 * @return: 0(성공 시작), -ENXIO(destruct 중), -EBUSY(resetting 중), -EALREADY(이미 disabled)
 *
 * nvme_ctrlr_op(NVME_CTRLR_OP_DISABLE) RPC에서 호출.
 * resetting=true, dont_retry=true로 설정 후 적절한 disable 함수를 msg_fn으로 선택:
 *   - reconnect 지연 중: _bdev_nvme_cancel_reconnect_and_disable_ctrlr
 *   - 그 외: _bdev_nvme_disconnect_and_disable_ctrlr
 * nvme_ctrlr 레퍼런스를 획득하고 spdk_thread_send_msg(app_thread)로 비동기 실행.
 * 컨텍스트: 어느 스레드에서도 호출 가능.
 *
 * 호출 체인:
 *   nvme_ctrlr_op() → [이 함수] → spdk_thread_send_msg → 선택된 msg_fn
 */
static int
bdev_nvme_disable_ctrlr(struct nvme_ctrlr *nvme_ctrlr)
{
	spdk_msg_fn msg_fn;

	pthread_mutex_lock(&nvme_ctrlr->mutex);
	if (nvme_ctrlr->destruct) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return -ENXIO;
	}

	if (nvme_ctrlr->resetting) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return -EBUSY;
	}

	if (nvme_ctrlr->disabled) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return -EALREADY;
	}

	nvme_ctrlr->resetting = true;
	nvme_ctrlr->dont_retry = true;

	if (nvme_ctrlr->reconnect_is_delayed) {
		msg_fn = _bdev_nvme_cancel_reconnect_and_disable_ctrlr;
		nvme_ctrlr->reconnect_is_delayed = false;
	} else {
		msg_fn = _bdev_nvme_disconnect_and_disable_ctrlr;
	}

	nvme_ctrlr->reset_start_tsc = spdk_get_ticks();

	nvme_ctrlr_get_ref(nvme_ctrlr);
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	spdk_thread_send_msg(spdk_thread_get_app_thread(), msg_fn, nvme_ctrlr);
	return 0;
}

/*
 * [한국어]
 * nvme_ctrlr_op - 단일 컨트롤러에 reset/enable/disable 중 하나를 적용하는 디스패처.
 *
 * @nvme_ctrlr: 대상 컨트롤러
 * @op: NVME_CTRLR_OP_RESET / _ENABLE / _DISABLE
 * @cb_fn: 완료 콜백 (성공 시 rc=0, 실패 시 rc<0)
 * @cb_arg: 콜백 인자
 * @return: 0(성공 시작), 에러코드
 *
 * op 값에 따라 bdev_nvme_reset_ctrlr / enable / disable_ctrlr를 호출하고
 * 성공 시 ctrlr_op_cb_fn/arg를 설정해 완료 시 콜백이 호출되게 한다.
 * -EALREADY는 호출자(nvme_ctrlr_op_rpc)에서 0으로 매핑된다.
 *
 * 호출 체인:
 *   nvme_ctrlr_op_rpc() / nvme_bdev_ctrlr_op_rpc() → [이 함수]
 *   → bdev_nvme_reset/enable/disable_ctrlr()
 */
static int
nvme_ctrlr_op(struct nvme_ctrlr *nvme_ctrlr, enum nvme_ctrlr_op op,
	      bdev_nvme_ctrlr_op_cb cb_fn, void *cb_arg)
{
	int rc;

	switch (op) {
	case NVME_CTRLR_OP_RESET:
		rc = bdev_nvme_reset_ctrlr(nvme_ctrlr);
		break;
	case NVME_CTRLR_OP_ENABLE:
		rc = bdev_nvme_enable_ctrlr(nvme_ctrlr);
		break;
	case NVME_CTRLR_OP_DISABLE:
		rc = bdev_nvme_disable_ctrlr(nvme_ctrlr);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	if (rc == 0) {
		assert(nvme_ctrlr->ctrlr_op_cb_fn == NULL);
		assert(nvme_ctrlr->ctrlr_op_cb_arg == NULL);
		nvme_ctrlr->ctrlr_op_cb_fn = cb_fn;
		nvme_ctrlr->ctrlr_op_cb_arg = cb_arg;
	}
	return rc;
}

/*
 * [한국어]
 * nvme_ctrlr_op_rpc_ctx - nvme_ctrlr_op_rpc() 및 nvme_bdev_ctrlr_op_rpc()의 실행 컨텍스트.
 *
 * reset/enable/disable 등의 컨트롤러 연산을 비동기 시퀀스로 수행할 때
 * 단일 nvme_ctrlr 또는 nvme_bdev_ctrlr 내 전체 컨트롤러를 순회하며 사용한다.
 */
struct nvme_ctrlr_op_rpc_ctx {
	struct nvme_ctrlr *nvme_ctrlr;
	/* [한국어] 현재 처리 중인 nvme_ctrlr 포인터.
	 * 설정자: nvme_bdev_ctrlr_op_rpc()의 초기화 및 _nvme_bdev_ctrlr_op_rpc_continue()의 순회.
	 * 읽는 자: nvme_ctrlr_op(), _nvme_bdev_ctrlr_op_rpc_continue()에서 다음 컨트롤러 탐색.
	 * 값 범위: 유효한 nvme_ctrlr 포인터 또는 NULL(완료 후).
	 * 동기화: app 스레드에서만 접근하므로 락 불필요. */

	enum nvme_ctrlr_op op;
	/* [한국어] 수행할 연산 종류 (RESET/ENABLE/DISABLE).
	 * 설정자: nvme_ctrlr_op_rpc() / nvme_bdev_ctrlr_op_rpc() 초기화 시.
	 * 읽는 자: _nvme_bdev_ctrlr_op_rpc_continue()에서 다음 컨트롤러에 같은 연산 적용.
	 * 값 범위: enum nvme_ctrlr_op 열거값 중 하나.
	 * 동기화: 단일 app 스레드에서만 접근. */

	int rc;
	/* [한국어] 현재까지의 누적 에러 코드.
	 * 설정자: nvme_bdev_ctrlr_op_rpc_continue()에서 각 컨트롤러 완료 시 갱신.
	 * 읽는 자: _nvme_bdev_ctrlr_op_rpc_continue()에서 에러 발생 여부 판단.
	 * 값 범위: 0(성공) 또는 음수 에러코드.
	 * 동기화: app 스레드에서만 접근. */

	bdev_nvme_ctrlr_op_cb cb_fn;
	/* [한국어] 모든 컨트롤러 연산 완료 후 호출할 최종 콜백 함수 포인터.
	 * 설정자: nvme_ctrlr_op_rpc() / nvme_bdev_ctrlr_op_rpc() 초기화 시.
	 * 읽는 자: nvme_ctrlr_op_rpc_complete(), _nvme_bdev_ctrlr_op_rpc_continue() 완료 시.
	 * 값 범위: NULL이 아닌 유효한 함수 포인터.
	 * 동기화: app 스레드에서만 접근. */

	void *cb_arg;
	/* [한국어] cb_fn에 전달할 불투명 콜백 인자 (일반적으로 RPC ctx 포인터).
	 * 설정자: nvme_ctrlr_op_rpc() / nvme_bdev_ctrlr_op_rpc() 초기화 시.
	 * 읽는 자: cb_fn 호출 시.
	 * 값 범위: NULL 또는 유효한 포인터.
	 * 동기화: app 스레드에서만 접근. */
};

/*
 * [한국어]
 * nvme_ctrlr_op_rpc_complete - 단일 컨트롤러 연산 완료 시 최종 콜백을 호출하고 ctx를 해제.
 *
 * @cb_arg: nvme_ctrlr_op_rpc_ctx 포인터
 * @rc: 연산 결과 코드
 * @return: void
 *
 * nvme_ctrlr_op()에 cb_fn으로 등록됨.
 * 연산 완료 후 ctx->cb_fn(ctx->cb_arg, rc)를 호출하고 ctx를 free한다.
 *
 * 호출 체인:
 *   nvme_ctrlr_op() 완료 → [이 함수] → ctx->cb_fn(cb_arg, rc)
 */
static void
nvme_ctrlr_op_rpc_complete(void *cb_arg, int rc)
{
	struct nvme_ctrlr_op_rpc_ctx *ctx = cb_arg;

	ctx->cb_fn(ctx->cb_arg, rc);
	free(ctx);
}

/*
 * [한국어]
 * nvme_ctrlr_op_rpc - bdev_nvme.h §2 참조. 단일 컨트롤러에 op(reset/enable/disable) 실행.
 *
 * RPC 핸들러가 직접 호출하는 진입점. ctx를 힙에 만들고 nvme_ctrlr_op로 비동기 시퀀스 시작,
 * 완료 시 cb_fn(cb_arg, rc)로 호출자에게 결과 전달. -EALREADY는 0(no-op 성공)으로 매핑.
 */
void
nvme_ctrlr_op_rpc(struct nvme_ctrlr *nvme_ctrlr, enum nvme_ctrlr_op op,
		  bdev_nvme_ctrlr_op_cb cb_fn, void *cb_arg)
{
	struct nvme_ctrlr_op_rpc_ctx *ctx;
	int rc;

	assert(cb_fn != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "Failed to allocate nvme_ctrlr_op_rpc_ctx.\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = nvme_ctrlr_op(nvme_ctrlr, op, nvme_ctrlr_op_rpc_complete, ctx);
	if (rc == 0) {
		return;
	} else if (rc == -EALREADY) {
		rc = 0;
	}

	nvme_ctrlr_op_rpc_complete(ctx, rc);
}

static void nvme_bdev_ctrlr_op_rpc_continue(void *cb_arg, int rc);

/*
 * [한국어]
 * _nvme_bdev_ctrlr_op_rpc_continue - app 스레드에서 다음 컨트롤러로 연산을 진행하는 내부 함수.
 *
 * @_ctx: nvme_ctrlr_op_rpc_ctx 포인터 (spdk_thread_send_msg 인자)
 * @return: void
 *
 * nvme_bdev_ctrlr_op_rpc_continue()에서 spdk_thread_send_msg(app_thread)로 디스패치됨.
 * ctx->rc가 에러면 즉시 cb_fn 호출로 완료.
 * 그렇지 않으면 TAILQ_NEXT로 다음 nvme_ctrlr를 찾아 같은 op를 실행.
 * 더 이상 컨트롤러가 없으면 cb_fn 호출로 완료.
 * 컨텍스트: app 스레드 강제.
 *
 * 호출 체인:
 *   nvme_bdev_ctrlr_op_rpc_continue() → spdk_thread_send_msg → [이 함수]
 *   → nvme_ctrlr_op(next_nvme_ctrlr) 또는 ctx->cb_fn()
 */
static void
_nvme_bdev_ctrlr_op_rpc_continue(void *_ctx)
{
	struct nvme_ctrlr_op_rpc_ctx *ctx = _ctx;
	struct nvme_ctrlr *prev_nvme_ctrlr, *next_nvme_ctrlr;
	int rc;

	prev_nvme_ctrlr = ctx->nvme_ctrlr;
	ctx->nvme_ctrlr = NULL;

	if (ctx->rc != 0) {
		goto complete;
	}

	next_nvme_ctrlr = TAILQ_NEXT(prev_nvme_ctrlr, tailq);
	if (next_nvme_ctrlr == NULL) {
		goto complete;
	}

	rc = nvme_ctrlr_op(next_nvme_ctrlr, ctx->op, nvme_bdev_ctrlr_op_rpc_continue, ctx);
	if (rc == 0) {
		ctx->nvme_ctrlr = next_nvme_ctrlr;
		return;
	} else if (rc == -EALREADY) {
		ctx->nvme_ctrlr = next_nvme_ctrlr;
		rc = 0;
	}

	ctx->rc = rc;

complete:
	ctx->cb_fn(ctx->cb_arg, ctx->rc);
	free(ctx);
}

/*
 * [한국어]
 * nvme_bdev_ctrlr_op_rpc_continue - 각 컨트롤러 연산 완료 시 app 스레드로 다음 진행을 dispatch.
 *
 * @cb_arg: nvme_ctrlr_op_rpc_ctx 포인터
 * @rc: 방금 완료된 컨트롤러 연산의 결과 코드
 * @return: void
 *
 * nvme_ctrlr_op()에 cb_fn으로 등록됨 (nvme_bdev_ctrlr_op_rpc에서 순회 시 사용).
 * rc를 ctx->rc에 저장하고 _nvme_bdev_ctrlr_op_rpc_continue를 app 스레드로 dispatch.
 * app 스레드 보장이 필요한 이유: TAILQ_NEXT 접근이 g_nvme_bdev_ctrlrs 리스트를 참조하기 때문.
 *
 * 호출 체인:
 *   nvme_ctrlr_op() 완료 → [이 함수] → spdk_thread_send_msg → _nvme_bdev_ctrlr_op_rpc_continue
 */
static void
nvme_bdev_ctrlr_op_rpc_continue(void *cb_arg, int rc)
{
	struct nvme_ctrlr_op_rpc_ctx *ctx = cb_arg;

	ctx->rc = rc;

	spdk_thread_send_msg(spdk_thread_get_app_thread(), _nvme_bdev_ctrlr_op_rpc_continue, ctx);
}

/*
 * [한국어]
 * nvme_bdev_ctrlr_op_rpc - bdev_nvme.h §2 참조. 멀티패스 그룹 내 모든 컨트롤러에 op 실행.
 *
 * 동작:
 *   1) ctx 할당, op/cb_fn/cb_arg 채움.
 *   2) 그룹의 첫 컨트롤러부터 nvme_ctrlr_op 시작 (continue 콜백으로 다음 컨트롤러로 진행).
 *   3) 모든 컨트롤러를 순차적으로 처리한 뒤 cb_fn 호출.
 * 도중 에러가 나면 즉시 중단하고 그 rc를 cb로 보고. 컨텍스트: app 스레드 강제.
 */
void
nvme_bdev_ctrlr_op_rpc(struct nvme_bdev_ctrlr *nbdev_ctrlr, enum nvme_ctrlr_op op,
		       bdev_nvme_ctrlr_op_cb cb_fn, void *cb_arg)
{
	struct nvme_ctrlr_op_rpc_ctx *ctx;
	struct nvme_ctrlr *nvme_ctrlr;
	int rc;

	assert(cb_fn != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate nvme_ctrlr_op_rpc_ctx.\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->op = op;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	nvme_ctrlr = TAILQ_FIRST(&nbdev_ctrlr->ctrlrs);
	assert(nvme_ctrlr != NULL);

	rc = nvme_ctrlr_op(nvme_ctrlr, op, nvme_bdev_ctrlr_op_rpc_continue, ctx);
	if (rc == 0) {
		ctx->nvme_ctrlr = nvme_ctrlr;
		return;
	} else if (rc == -EALREADY) {
		ctx->nvme_ctrlr = nvme_ctrlr;
		rc = 0;
	}

	nvme_bdev_ctrlr_op_rpc_continue(ctx, rc);
}

static int _bdev_nvme_reset_io(struct nvme_io_path *io_path, struct nvme_bdev_io *bio);

/*
 * [한국어]
 * bdev_nvme_unfreeze_bdev_channel_done - 모든 채널의 unfreeze 완료 후 reset_io를 최종 완료.
 *
 * @nbdev: 대상 NVMe bdev
 * @ctx: nvme_bdev_io (reset I/O) 포인터
 * @status: 0이면 전체 채널 unfreeze 성공
 * @return: void
 *
 * nvme_bdev_for_each_channel(bdev_nvme_unfreeze_bdev_channel)의 완료 콜백.
 * bio->cpl.cdw0 값으로 최종 성공/실패 상태를 결정하고 __bdev_nvme_io_complete를 호출한다.
 * cdw0==0이면 SUCCESS, 비0이면 FAILED.
 *
 * 호출 체인:
 *   nvme_bdev_for_each_channel(bdev_nvme_unfreeze_bdev_channel) → [이 함수]
 *   → __bdev_nvme_io_complete(bdev_io, io_status)
 */
static void
bdev_nvme_unfreeze_bdev_channel_done(struct nvme_bdev *nbdev, void *ctx, int status)
{
	struct nvme_bdev_io *bio = ctx;
	enum spdk_bdev_io_status io_status;

	if (bio->cpl.cdw0 == 0) {
		io_status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else {
		io_status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	NVME_BDEV_INFOLOG(nbdev, null_ctrlr, "reset_io %p completed, status:%d\n", bio, io_status);
	__bdev_nvme_io_complete(spdk_bdev_io_from_ctx(bio), io_status, NULL);
}

/*
 * [한국어]
 * bdev_nvme_unfreeze_bdev_channel - reset_io 완료 후 각 채널의 freeze를 해제.
 *
 * @i: 채널 이터레이터
 * @nbdev: 대상 NVMe bdev
 * @nbdev_ch: 현재 처리 중인 bdev 채널
 * @ctx: 사용하지 않음
 * @return: void
 *
 * nvme_bdev_for_each_channel의 채널별 콜백.
 * retry 대기 중인 I/O를 모두 abort하고 resetting=false로 채널을 unfreeze한다.
 * 채널이 unfreeze되면 새로운 I/O가 다시 처리될 수 있게 된다.
 *
 * 호출 체인:
 *   bdev_nvme_reset_io_complete() → nvme_bdev_for_each_channel → [이 함수]
 *   → bdev_nvme_unfreeze_bdev_channel_done()
 */
static void
bdev_nvme_unfreeze_bdev_channel(struct nvme_bdev_channel_iter *i,
				struct nvme_bdev *nbdev,
				struct nvme_bdev_channel *nbdev_ch, void *ctx)
{
	bdev_nvme_abort_retry_ios(nbdev_ch);
	nbdev_ch->resetting = false;

	nvme_bdev_for_each_channel_continue(i, 0);
}

/*
 * [한국어]
 * bdev_nvme_reset_io_complete - 모든 nvme_ctrlr 리셋 시도 완료 후 채널 unfreeze 단계로 진입.
 *
 * @bio: reset I/O 포인터 (nvme_bdev_io)
 * @return: void
 *
 * _bdev_nvme_reset_io_continue()에서 더 이상 처리할 io_path가 없을 때 호출.
 * nvme_bdev_for_each_channel(bdev_nvme_unfreeze_bdev_channel)을 통해 모든 채널을
 * unfreeze하고 retry 대기 I/O를 abort한다.
 *
 * 호출 체인:
 *   _bdev_nvme_reset_io_continue() → [이 함수]
 *   → nvme_bdev_for_each_channel(bdev_nvme_unfreeze_bdev_channel)
 *   → bdev_nvme_unfreeze_bdev_channel_done()
 */
static void
bdev_nvme_reset_io_complete(struct nvme_bdev_io *bio)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	struct nvme_bdev *nbdev = (struct nvme_bdev *)bdev_io->bdev->ctxt;

	/* Abort all queued I/Os for retry. */
	nvme_bdev_for_each_channel(nbdev,
				   bdev_nvme_unfreeze_bdev_channel,
				   bio,
				   bdev_nvme_unfreeze_bdev_channel_done);
}

/*
 * [한국어]
 * _bdev_nvme_reset_io_continue - I/O 요청 스레드에서 다음 io_path의 리셋을 계속 진행.
 *
 * @ctx: nvme_bdev_io 포인터 (spdk_thread_send_msg 인자)
 * @return: void
 *
 * bdev_nvme_reset_io_continue()에서 spdk_thread_send_msg로 I/O 스레드에서 실행됨.
 * 이전 io_path를 완료하고 STAILQ_NEXT로 다음 io_path를 찾아 _bdev_nvme_reset_io를 호출.
 * 더 이상 io_path가 없으면 bdev_nvme_reset_io_complete로 전체 리셋 완료.
 *
 * 호출 체인:
 *   bdev_nvme_reset_io_continue() → spdk_thread_send_msg → [이 함수]
 *   → _bdev_nvme_reset_io(next) 또는 bdev_nvme_reset_io_complete()
 */
static void
_bdev_nvme_reset_io_continue(void *ctx)
{
	struct nvme_bdev_io *bio = ctx;
	struct nvme_io_path *prev_io_path, *next_io_path;
	int rc;

	prev_io_path = bio->io_path;
	bio->io_path = NULL;

	next_io_path = STAILQ_NEXT(prev_io_path, stailq);
	if (next_io_path == NULL) {
		goto complete;
	}

	rc = _bdev_nvme_reset_io(next_io_path, bio);
	if (rc == 0) {
		return;
	}

complete:
	bdev_nvme_reset_io_complete(bio);
}

/*
 * [한국어]
 * bdev_nvme_reset_io_continue - 각 nvme_ctrlr 리셋 완료 후 I/O 스레드로 다음 단계를 dispatch.
 *
 * @cb_arg: nvme_bdev_io 포인터 (reset I/O)
 * @rc: 방금 완료된 컨트롤러 리셋 결과 코드 (0=성공)
 * @return: void
 *
 * _bdev_nvme_reset_io()에서 ctrlr_op_cb_fn으로 등록됨.
 * rc==0이면 bio->cpl.cdw0=0(성공으로 표시)하고
 * I/O 스레드에 _bdev_nvme_reset_io_continue를 dispatch한다.
 * I/O 스레드에서 dispatch하는 이유: STAILQ_NEXT 접근이 bdev_channel 데이터이기 때문.
 *
 * 호출 체인:
 *   bdev_nvme_reset_ctrlr_complete() 또는 bdev_nvme_complete_pending_resets() → ctrlr_op_cb_fn
 *   → [이 함수] → spdk_thread_send_msg(I/O thread) → _bdev_nvme_reset_io_continue
 */
static void
bdev_nvme_reset_io_continue(void *cb_arg, int rc)
{
	struct nvme_bdev_io *bio = cb_arg;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	struct nvme_bdev *nbdev = (struct nvme_bdev *)bdev_io->bdev->ctxt;

	NVME_BDEV_INFOLOG(nbdev, null_ctrlr, "continue reset_io %p, rc:%d\n", bio, rc);

	/* Reset status is initialized as "failed". Set to "success" once we have at least one
	 * successfully reset nvme_ctrlr.
	 */
	if (rc == 0) {
		bio->cpl.cdw0 = 0;
	}

	spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io), _bdev_nvme_reset_io_continue, bio);
}

/*
 * [한국어]
 * _bdev_nvme_reset_io - 특정 io_path의 nvme_ctrlr에 대해 리셋을 시작하거나 대기열에 추가.
 *
 * @io_path: 리셋할 대상 nvme_ctrlr를 가리키는 io_path
 * @bio: reset I/O 요청 (nvme_bdev_io)
 * @return: 0(리셋 시작 또는 pending 큐 추가), 음수 에러코드(-EALREADY=disabled)
 *
 * bdev_nvme_freeze_bdev_channel_done()과 _bdev_nvme_reset_io_continue()에서 호출.
 * bdev_nvme_reset_ctrlr_unsafe()로 리셋 플래그를 설정하고:
 *   - 성공: ctrlr_op_cb_fn=bdev_nvme_reset_io_continue로 설정하고 msg_fn dispatch.
 *   - -EBUSY: pending_resets 큐에 추가하고 0 반환.
 *   - -EALREADY: disabled 상태 → 호출자가 건너뜀.
 *
 * 호출 체인:
 *   bdev_nvme_freeze_bdev_channel_done() → [이 함수]
 *   또는 _bdev_nvme_reset_io_continue() → [이 함수]
 *   → spdk_thread_send_msg(app_thread, msg_fn)
 */
static int
_bdev_nvme_reset_io(struct nvme_io_path *io_path, struct nvme_bdev_io *bio)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	struct nvme_bdev *nbdev = (struct nvme_bdev *)bdev_io->bdev->ctxt;
	struct nvme_ctrlr *nvme_ctrlr = io_path->qpair->ctrlr;
	spdk_msg_fn msg_fn;
	int rc;

	assert(bio->io_path == NULL);
	bio->io_path = io_path;

	pthread_mutex_lock(&nvme_ctrlr->mutex);
	rc = bdev_nvme_reset_ctrlr_unsafe(nvme_ctrlr, &msg_fn);
	if (rc == -EBUSY) {
		/*
		 * Reset call is queued only if it is from the app framework. This is on purpose so that
		 * we don't interfere with the app framework reset strategy. i.e. we are deferring to the
		 * upper level. If they are in the middle of a reset, we won't try to schedule another one.
		 */
		TAILQ_INSERT_TAIL(&nvme_ctrlr->pending_resets, bio, retry_link);
	}
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	if (rc == 0) {
		assert(nvme_ctrlr->ctrlr_op_cb_fn == NULL);
		assert(nvme_ctrlr->ctrlr_op_cb_arg == NULL);
		nvme_ctrlr->ctrlr_op_cb_fn = bdev_nvme_reset_io_continue;
		nvme_ctrlr->ctrlr_op_cb_arg = bio;

		spdk_thread_send_msg(spdk_thread_get_app_thread(), msg_fn, nvme_ctrlr);

		NVME_BDEV_INFOLOG(nbdev, nvme_ctrlr, "reset_io %p started resetting ctrlr.\n", bio);
	} else if (rc == -EBUSY) {
		rc = 0;
		NVME_BDEV_INFOLOG(nbdev, nvme_ctrlr, "reset_io %p was queued to ctrlr.\n", bio);
	} else {
		NVME_BDEV_INFOLOG(nbdev, nvme_ctrlr, "reset_io %p could not reset ctrlr, rc:%d\n", bio, rc);
	}

	return rc;
}

/*
 * [한국어]
 * bdev_nvme_freeze_bdev_channel_done - 모든 채널 freeze 완료 후 컨트롤러 리셋 시퀀스 시작.
 *
 * @nbdev: 대상 NVMe bdev
 * @ctx: nvme_bdev_io (reset I/O) 포인터
 * @status: 0이면 전체 채널 freeze 성공
 * @return: void
 *
 * nvme_bdev_for_each_channel(bdev_nvme_freeze_bdev_channel)의 완료 콜백.
 * bio->cpl.cdw0=1(초기 실패 상태)로 설정하고 첫 번째 io_path부터
 * _bdev_nvme_reset_io()를 통해 각 nvme_ctrlr 리셋을 순차적으로 시작한다.
 * 첫 io_path가 disabled(-EALREADY)이면 bdev_nvme_reset_io_continue로 건너뛴다.
 *
 * 호출 체인:
 *   nvme_bdev_for_each_channel(bdev_nvme_freeze_bdev_channel) → [이 함수]
 *   → _bdev_nvme_reset_io(first io_path)
 */
static void
bdev_nvme_freeze_bdev_channel_done(struct nvme_bdev *nbdev, void *ctx, int status)
{
	struct nvme_bdev_io *bio = ctx;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_io_path *io_path;
	int rc;

	nbdev_ch = spdk_io_channel_get_ctx(spdk_bdev_io_get_io_channel(bdev_io));

	/* Initialize with failed status. With multipath it is enough to have at least one successful
	 * nvme_ctrlr reset. If there is none, reset status will remain failed.
	 */
	bio->cpl.cdw0 = 1;

	/* Reset all nvme_ctrlrs of a bdev controller sequentially. */
	io_path = STAILQ_FIRST(&nbdev_ch->io_path_list);
	assert(io_path != NULL);

	rc = _bdev_nvme_reset_io(io_path, bio);
	if (rc != 0) {
		/* If the current nvme_ctrlr is disabled, skip it and move to the next nvme_ctrlr. */
		rc = (rc == -EALREADY) ? 0 : rc;

		bdev_nvme_reset_io_continue(bio, rc);
	}
}

/*
 * [한국어]
 * bdev_nvme_freeze_bdev_channel - reset_io 시작 시 각 채널을 freeze(새 I/O 차단).
 *
 * @i: 채널 이터레이터
 * @nbdev: 대상 NVMe bdev
 * @nbdev_ch: 현재 처리 중인 bdev 채널
 * @ctx: 사용하지 않음
 * @return: void
 *
 * nvme_bdev_for_each_channel의 채널별 콜백.
 * resetting=true로 설정해 bdev_nvme_find_io_path()가 이 채널에서 새 I/O 경로를 찾지 못하게 한다.
 * 이미 진행 중인 I/O는 계속 처리되지만 새 I/O는 retry 대기열로 들어간다.
 *
 * 호출 체인:
 *   bdev_nvme_reset_io() → nvme_bdev_for_each_channel → [이 함수]
 *   → bdev_nvme_freeze_bdev_channel_done()
 */
static void
bdev_nvme_freeze_bdev_channel(struct nvme_bdev_channel_iter *i,
			      struct nvme_bdev *nbdev,
			      struct nvme_bdev_channel *nbdev_ch, void *ctx)
{
	nbdev_ch->resetting = true;

	nvme_bdev_for_each_channel_continue(i, 0);
}

/*
 * [한국어]
 * bdev_nvme_reset_io - bdev RESET I/O 요청의 진입점. 모든 채널을 freeze하고 리셋 시퀀스 시작.
 *
 * @nbdev: 리셋할 NVMe bdev
 * @bio: RESET I/O 요청 (nvme_bdev_io), bdev_io->type == SPDK_BDEV_IO_TYPE_RESET
 * @return: void
 *
 * _bdev_nvme_submit_request()에서 SPDK_BDEV_IO_TYPE_RESET 케이스로 호출.
 * nvme_bdev_for_each_channel(bdev_nvme_freeze_bdev_channel)을 통해 모든 채널의
 * resetting=true로 설정하고 bdev_nvme_freeze_bdev_channel_done에서 실제 리셋 시작.
 * 멀티패스 환경에서는 io_path_list의 모든 nvme_ctrlr를 순차적으로 리셋한다.
 *
 * 호출 체인:
 *   _bdev_nvme_submit_request(RESET) → [이 함수]
 *   → nvme_bdev_for_each_channel(bdev_nvme_freeze_bdev_channel)
 *   → bdev_nvme_freeze_bdev_channel_done() → _bdev_nvme_reset_io()
 */
static void
bdev_nvme_reset_io(struct nvme_bdev *nbdev, struct nvme_bdev_io *bio)
{
	NVME_BDEV_INFOLOG(nbdev, null_ctrlr, "reset_io %p started.\n", bio);
	nvme_bdev_for_each_channel(nbdev,
				   bdev_nvme_freeze_bdev_channel,
				   bio,
				   bdev_nvme_freeze_bdev_channel_done);
}

/*
 * [한국어]
 * bdev_nvme_failover_ctrlr_unsafe - mutex 보유 상태에서 페일오버 플래그를 설정하는 내부 함수.
 *
 * @nvme_ctrlr: 페일오버할 컨트롤러
 * @remove: true이면 현재 trid를 제거(삭제 케이스), false이면 실패 표시만
 * @return: 0(페일오버 시작), -ENXIO(destruct 중), -EINPROGRESS(reset 중),
 *          -EBUSY(페일오버 이미 진행 중), -EALREADY(지연 재연결 또는 disabled)
 *
 * bdev_nvme_failover_ctrlr()와 _bdev_nvme_delete()에서 mutex 보유 상태로 호출됨.
 * bdev_nvme_failover_trid()로 다음 trid를 활성으로 설정하고
 * resetting=true, in_failover=true 플래그를 설정한다.
 * 이미 reconnect 지연 중이거나 disabled면 현재 타이머/활성화에 의존해 -EALREADY 반환.
 *
 * 호출 체인:
 *   bdev_nvme_failover_ctrlr() → [이 함수]
 *   또는 _bdev_nvme_delete() → [이 함수] → (0이면) _bdev_nvme_reset_ctrlr dispatch
 */
static int
bdev_nvme_failover_ctrlr_unsafe(struct nvme_ctrlr *nvme_ctrlr, bool remove)
{
	if (nvme_ctrlr->destruct) {
		/* Don't bother resetting if the controller is in the process of being destructed. */
		return -ENXIO;
	}

	if (nvme_ctrlr->resetting) {
		if (!nvme_ctrlr->in_failover) {
			NVME_CTRLR_NOTICELOG(nvme_ctrlr,
					     "Reset is already in progress. Defer failover until reset completes.\n");

			/* Defer failover until reset completes. */
			nvme_ctrlr->pending_failover = true;
			return -EINPROGRESS;
		} else {
			NVME_CTRLR_NOTICELOG(nvme_ctrlr, "Unable to perform failover, already in progress.\n");
			return -EBUSY;
		}
	}

	bdev_nvme_failover_trid(nvme_ctrlr, remove, true);

	if (nvme_ctrlr->reconnect_is_delayed) {
		NVME_CTRLR_NOTICELOG(nvme_ctrlr, "Reconnect is already scheduled.\n");

		/* We rely on the next reconnect for the failover. */
		return -EALREADY;
	}

	if (nvme_ctrlr->disabled) {
		NVME_CTRLR_NOTICELOG(nvme_ctrlr, "Controller is disabled.\n");

		/* We rely on the enablement for the failover. */
		return -EALREADY;
	}

	nvme_ctrlr->resetting = true;
	nvme_ctrlr->in_failover = true;

	if (nvme_ctrlr->reset_start_tsc == 0) {
		nvme_ctrlr->reset_start_tsc = spdk_get_ticks();
	}

	return 0;
}

/*
 * [한국어]
 * bdev_nvme_failover_ctrlr - thread-safe 페일오버 진입점: mutex 획득 후 페일오버 시작.
 *
 * @nvme_ctrlr: 페일오버할 컨트롤러
 * @return: 0(성공 또는 -EALREADY가 무시됨), 에러코드
 *
 * bdev_nvme_disconnected_qpair_cb()와 bdev_nvme_check_ctrlr_loss_timeout() 등에서 호출.
 * mutex를 획득해 bdev_nvme_failover_ctrlr_unsafe(remove=false)를 호출하고
 * 성공 시 spdk_thread_send_msg(app_thread, _bdev_nvme_reset_ctrlr)로 리셋 시작.
 * -EALREADY는 0으로 처리(이미 재연결 예약됨).
 * 컨텍스트: 어느 스레드에서도 호출 가능.
 *
 * 호출 체인:
 *   bdev_nvme_disconnected_qpair_cb() → [이 함수]
 *   → spdk_thread_send_msg(app_thread) → _bdev_nvme_reset_ctrlr
 */
static int
bdev_nvme_failover_ctrlr(struct nvme_ctrlr *nvme_ctrlr)
{
	int rc;

	pthread_mutex_lock(&nvme_ctrlr->mutex);
	rc = bdev_nvme_failover_ctrlr_unsafe(nvme_ctrlr, false);
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	if (rc == 0) {
		spdk_thread_send_msg(spdk_thread_get_app_thread(), _bdev_nvme_reset_ctrlr, nvme_ctrlr);
	} else if (rc == -EALREADY) {
		rc = 0;
	}

	return rc;
}

static int bdev_nvme_unmap(struct nvme_bdev_io *bio, uint64_t offset_blocks,
			   uint64_t num_blocks);

static int bdev_nvme_write_zeroes(struct nvme_bdev_io *bio, uint64_t offset_blocks,
				  uint64_t num_blocks);

static int bdev_nvme_flush(struct nvme_bdev_io *bio);

static int bdev_nvme_copy(struct nvme_bdev_io *bio, uint64_t dst_offset_blocks,
			  uint64_t src_offset_blocks,
			  uint64_t num_blocks);

/*
 * [한국어]
 * bdev_nvme_get_buf_cb - spdk_bdev_io_get_buf() 완료 콜백: 버퍼 획득 후 readv 실행.
 *
 * @ch: I/O 채널
 * @bdev_io: 버퍼를 기다리던 bdev I/O 요청
 * @success: 버퍼 획득 성공 여부
 * @return: void
 *
 * READ I/O에서 iov_base가 NULL인 경우(버퍼 미할당) spdk_bdev_io_get_buf()를 호출하고
 * 이 함수가 버퍼 할당 완료 후 콜백으로 호출된다.
 * 버퍼 획득 실패 시 -EINVAL로 실패 완료.
 * io_path가 사용 불가이면 -ENXIO로 실패.
 * 성공 시 bdev_nvme_readv()를 호출해 실제 NVMe READ 명령을 제출.
 *
 * 호출 체인:
 *   spdk_bdev_io_get_buf() → (버퍼 할당) → [이 함수] → bdev_nvme_readv()
 */
static void
bdev_nvme_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		     bool success)
{
	struct nvme_bdev_io *bio = (struct nvme_bdev_io *)bdev_io->driver_ctx;
	int ret;

	if (!success) {
		ret = -EINVAL;
		goto exit;
	}

	if (spdk_unlikely(!nvme_io_path_is_available(bio->io_path))) {
		ret = -ENXIO;
		goto exit;
	}

	ret = bdev_nvme_readv(bio,
			      bdev_io->u.bdev.iovs,
			      bdev_io->u.bdev.iovcnt,
			      bdev_io->u.bdev.md_buf,
			      bdev_io->u.bdev.num_blocks,
			      bdev_io->u.bdev.offset_blocks,
			      bdev_io->u.bdev.dif_check_flags,
			      bdev_io->u.bdev.memory_domain,
			      bdev_io->u.bdev.memory_domain_ctx,
			      bdev_io->u.bdev.accel_sequence);

exit:
	if (spdk_unlikely(ret != 0)) {
		bdev_nvme_io_complete(bio, ret);
	}
}

/*
 * [한국어]
 * _bdev_nvme_submit_request - I/O 타입에 따라 실제 NVMe 명령을 제출하는 내부 디스패처.
 *
 * @nbdev_ch: bdev 채널 (I/O 경로 정보 포함)
 * @bdev_io: 제출할 bdev I/O 요청
 * @return: void
 *
 * bdev_nvme_submit_request()에서 호출. bdev_io->type에 따라 다음 중 하나를 실행:
 *   READ: iov_base가 있으면 bdev_nvme_readv(), 없으면 spdk_bdev_io_get_buf() (콜백으로 재진입).
 *   WRITE: bdev_nvme_writev()
 *   COMPARE, COMPARE_AND_WRITE, UNMAP, WRITE_ZEROES, ZONE_APPEND, GET_ZONE_INFO,
 *   ZONE_MANAGEMENT, COPY, WRITE_UNCORRECTABLE: 각 해당 함수 호출.
 *   RESET: io_path=NULL로 설정 후 bdev_nvme_reset_io(). return(완료 처리 다름).
 *   NVME_NSSR: 서브시스템 리셋 후 즉시 완료.
 *   FLUSH: VWC 비활성 또는 g_opts.enable_flush=false이면 즉시 성공.
 *   NVME_ADMIN: io_path=NULL로 bdev_nvme_admin_passthru(). return.
 *   NVME_IO, NVME_IO_MD, NVME_IOV_MD: passthru 함수 호출.
 *   ABORT: bio_to_abort를 통해 bdev_nvme_abort(). return.
 * rc!=0이면 bdev_nvme_io_complete(bio, rc)로 실패 처리.
 *
 * 호출 체인:
 *   bdev_nvme_submit_request() → [이 함수] → 각 I/O 타입별 함수
 */
static inline void
_bdev_nvme_submit_request(struct nvme_bdev_channel *nbdev_ch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_bdev_io *nbdev_io = (struct nvme_bdev_io *)bdev_io->driver_ctx;
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct nvme_bdev_io *nbdev_io_to_abort;
	int rc = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs && bdev_io->u.bdev.iovs[0].iov_base) {

			rc = bdev_nvme_readv(nbdev_io,
					     bdev_io->u.bdev.iovs,
					     bdev_io->u.bdev.iovcnt,
					     bdev_io->u.bdev.md_buf,
					     bdev_io->u.bdev.num_blocks,
					     bdev_io->u.bdev.offset_blocks,
					     bdev_io->u.bdev.dif_check_flags,
					     bdev_io->u.bdev.memory_domain,
					     bdev_io->u.bdev.memory_domain_ctx,
					     bdev_io->u.bdev.accel_sequence);
		} else {
			spdk_bdev_io_get_buf(bdev_io, bdev_nvme_get_buf_cb,
					     bdev_io->u.bdev.num_blocks * bdev->blocklen);
			rc = 0;
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = bdev_nvme_writev(nbdev_io,
				      bdev_io->u.bdev.iovs,
				      bdev_io->u.bdev.iovcnt,
				      bdev_io->u.bdev.md_buf,
				      bdev_io->u.bdev.num_blocks,
				      bdev_io->u.bdev.offset_blocks,
				      bdev_io->u.bdev.dif_check_flags,
				      bdev_io->u.bdev.memory_domain,
				      bdev_io->u.bdev.memory_domain_ctx,
				      bdev_io->u.bdev.accel_sequence,
				      bdev_io->u.bdev.nvme_cdw12,
				      bdev_io->u.bdev.nvme_cdw13);
		break;
	case SPDK_BDEV_IO_TYPE_COMPARE:
		rc = bdev_nvme_comparev(nbdev_io,
					bdev_io->u.bdev.iovs,
					bdev_io->u.bdev.iovcnt,
					bdev_io->u.bdev.md_buf,
					bdev_io->u.bdev.num_blocks,
					bdev_io->u.bdev.offset_blocks,
					bdev_io->u.bdev.dif_check_flags);
		break;
	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
		rc = bdev_nvme_comparev_and_writev(nbdev_io,
						   bdev_io->u.bdev.iovs,
						   bdev_io->u.bdev.iovcnt,
						   bdev_io->u.bdev.fused_iovs,
						   bdev_io->u.bdev.fused_iovcnt,
						   bdev_io->u.bdev.md_buf,
						   bdev_io->u.bdev.num_blocks,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.dif_check_flags);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = bdev_nvme_unmap(nbdev_io,
				     bdev_io->u.bdev.offset_blocks,
				     bdev_io->u.bdev.num_blocks);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc =  bdev_nvme_write_zeroes(nbdev_io,
					     bdev_io->u.bdev.offset_blocks,
					     bdev_io->u.bdev.num_blocks);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		nbdev_io->io_path = NULL;
		bdev_nvme_reset_io(bdev->ctxt, nbdev_io);
		return;

	case SPDK_BDEV_IO_TYPE_NVME_NSSR:
		spdk_nvme_ctrlr_reset_subsystem(nbdev_io->io_path->qpair->ctrlr->ctrlr);
		bdev_nvme_io_complete(nbdev_io, 0);
		return;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		/* No need to send flush if Volatile Write Cache is disabled */
		if (!bdev->write_cache || !g_opts.enable_flush) {
			bdev_nvme_io_complete(nbdev_io, 0);
			return;
		}

		rc = bdev_nvme_flush(nbdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_ZONE_APPEND:
		rc = bdev_nvme_zone_appendv(nbdev_io,
					    bdev_io->u.bdev.iovs,
					    bdev_io->u.bdev.iovcnt,
					    bdev_io->u.bdev.md_buf,
					    bdev_io->u.bdev.num_blocks,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.dif_check_flags);
		break;
	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
		rc = bdev_nvme_get_zone_info(nbdev_io,
					     bdev_io->u.zone_mgmt.zone_id,
					     bdev_io->u.zone_mgmt.num_zones,
					     bdev_io->u.zone_mgmt.buf);
		break;
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		rc = bdev_nvme_zone_management(nbdev_io,
					       bdev_io->u.zone_mgmt.zone_id,
					       bdev_io->u.zone_mgmt.zone_action);
		break;
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
		nbdev_io->io_path = NULL;
		bdev_nvme_admin_passthru(nbdev_ch,
					 nbdev_io,
					 &bdev_io->u.nvme_passthru.cmd,
					 bdev_io->u.nvme_passthru.buf,
					 bdev_io->u.nvme_passthru.nbytes);
		return;

	case SPDK_BDEV_IO_TYPE_NVME_IO:
		rc = bdev_nvme_io_passthru(nbdev_io,
					   &bdev_io->u.nvme_passthru.cmd,
					   bdev_io->u.nvme_passthru.buf,
					   bdev_io->u.nvme_passthru.nbytes);
		break;
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		rc = bdev_nvme_io_passthru_md(nbdev_io,
					      &bdev_io->u.nvme_passthru.cmd,
					      bdev_io->u.nvme_passthru.buf,
					      bdev_io->u.nvme_passthru.nbytes,
					      bdev_io->u.nvme_passthru.md_buf,
					      bdev_io->u.nvme_passthru.md_len);
		break;
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
		rc = bdev_nvme_iov_passthru_md(nbdev_io,
					       &bdev_io->u.nvme_passthru.cmd,
					       bdev_io->u.nvme_passthru.iovs,
					       bdev_io->u.nvme_passthru.iovcnt,
					       bdev_io->u.nvme_passthru.nbytes,
					       bdev_io->u.nvme_passthru.md_buf,
					       bdev_io->u.nvme_passthru.md_len);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		nbdev_io->io_path = NULL;
		nbdev_io_to_abort = (struct nvme_bdev_io *)bdev_io->u.abort.bio_to_abort->driver_ctx;
		bdev_nvme_abort(nbdev_ch,
				nbdev_io,
				nbdev_io_to_abort);
		return;

	case SPDK_BDEV_IO_TYPE_COPY:
		rc = bdev_nvme_copy(nbdev_io,
				    bdev_io->u.bdev.offset_blocks,
				    bdev_io->u.bdev.copy.src_offset_blocks,
				    bdev_io->u.bdev.num_blocks);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_UNCORRECTABLE:
		rc = bdev_nvme_write_uncorrectable(nbdev_io,
						   bdev_io->u.bdev.num_blocks,
						   bdev_io->u.bdev.offset_blocks);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	if (spdk_unlikely(rc != 0)) {
		bdev_nvme_io_complete(nbdev_io, rc);
	}
}

/*
 * [한국어]
 * bdev_nvme_submit_request - bdev 레이어에서 호출하는 I/O 제출 함수 (retry 경로).
 *
 * @ch: spdk_io_channel (spdk_io_channel_get_ctx로 nvme_bdev_channel 획득)
 * @bdev_io: 제출할 bdev I/O 요청
 * @return: void
 *
 * bdev_nvme_submit_request_initial()에서 submit_tsc/retry_count 초기화 후 호출되거나
 * retry 경로에서 직접 호출됨.
 * submit_tsc: 최초 제출이면 bdev_io의 타임스탬프 사용, retry이면 현재 tsc.
 * spdk_trace_record로 TRACE_BDEV_NVME_IO_START 기록.
 * bdev_nvme_find_io_path()로 최적 io_path 탐색 후 _bdev_nvme_submit_request() 호출.
 * admin 명령은 io_path가 없어도 허용(fallthrough).
 * 일반 I/O에서 io_path 없으면 -ENXIO로 실패.
 *
 * 호출 체인:
 *   bdev_nvme_submit_request_initial() → [이 함수] → _bdev_nvme_submit_request()
 *   또는 bdev_nvme_retry_io() → [이 함수] → _bdev_nvme_submit_request()
 */
static void
bdev_nvme_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_bdev_channel *nbdev_ch = spdk_io_channel_get_ctx(ch);
	struct nvme_bdev_io *nbdev_io = (struct nvme_bdev_io *)bdev_io->driver_ctx;

	if (spdk_likely(nbdev_io->submit_tsc == 0)) {
		nbdev_io->submit_tsc = spdk_bdev_io_get_submit_tsc(bdev_io);
	} else {
		/* There are cases where submit_tsc != 0, i.e. retry I/O.
		 * We need to update submit_tsc here.
		 */
		nbdev_io->submit_tsc = spdk_get_ticks();
	}

	spdk_trace_record(TRACE_BDEV_NVME_IO_START, 0, 0, (uintptr_t)nbdev_io, (uintptr_t)bdev_io);
	nbdev_io->io_path = bdev_nvme_find_io_path(nbdev_ch);
	if (spdk_unlikely(!nbdev_io->io_path)) {
		if (!bdev_nvme_io_type_is_admin(bdev_io->type)) {
			bdev_nvme_io_complete(nbdev_io, -ENXIO);
			return;
		}

		/* Admin commands do not use the optimal I/O path.
		 * Simply fall through even if it is not found.
		 */
	}

	_bdev_nvme_submit_request(nbdev_ch, bdev_io);
}

/*
 * [한국어]
 * bdev_nvme_submit_request_initial - 새 I/O의 실제 최초 제출 진입점 (nvmelib_fn_table.submit_request).
 *
 * @ch: spdk_io_channel
 * @bdev_io: 최초 제출되는 bdev I/O
 * @return: void
 *
 * nvmelib_fn_table.submit_request로 등록된 함수.
 * bdev 레이어가 새 I/O를 제출할 때 이 함수가 호출된다.
 * submit_tsc=0, retry_count=0으로 초기화한 후 bdev_nvme_submit_request()를 호출.
 * retry 경로(bdev_nvme_retry_io)에서는 이 함수가 아닌 bdev_nvme_submit_request()를 직접 호출.
 *
 * 호출 체인:
 *   bdev 레이어(nvmelib_fn_table.submit_request) → [이 함수] → bdev_nvme_submit_request()
 */
static void
bdev_nvme_submit_request_initial(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_bdev_io *nbdev_io = (struct nvme_bdev_io *)bdev_io->driver_ctx;

	/* Initialize our values of submit tsc and retry count here
	 * so that it doesn't interfere with the retry process
	 */
	nbdev_io->submit_tsc = 0;
	nbdev_io->retry_count = 0;

	bdev_nvme_submit_request(ch, bdev_io);
}

/*
 * [한국어]
 * bdev_nvme_is_supported_csi - 주어진 NVMe CSI(Command Set Identifier)가 지원되는지 확인.
 *
 * @csi: NVMe Command Set Identifier (SPDK_NVME_CSI_NVM, SPDK_NVME_CSI_ZNS 등)
 * @return: true이면 지원, false이면 미지원
 *
 * bdev_nvme_io_type_supported()에서 호출해 ZNS/NVM 외의 CSI를 처리하는 논리 분기에 사용.
 * NVM(일반 블록)과 ZNS(Zoned Namespace) CSI만 지원한다.
 *
 * 호출 체인:
 *   bdev_nvme_io_type_supported() → [이 함수]
 */
static bool
bdev_nvme_is_supported_csi(enum spdk_nvme_csi csi)
{
	switch (csi) {
	case SPDK_NVME_CSI_NVM:
		return true;
	case SPDK_NVME_CSI_ZNS:
		return true;
	default:
		return false;
	}
}

/*
 * [한국어]
 * bdev_nvme_io_type_supported - 이 bdev이 특정 I/O 타입을 지원하는지 여부를 반환.
 *
 * @ctx: nvme_bdev 포인터 (bdev->ctxt)
 * @io_type: 확인할 I/O 타입 (SPDK_BDEV_IO_TYPE_*)
 * @return: true이면 지원, false이면 미지원
 *
 * nvmelib_fn_table.io_type_supported로 등록. bdev 레이어가 특정 I/O 타입 가용성을 확인 시 호출.
 * 지원되지 않는 CSI면 NVME_ADMIN/NVME_IO/NVME_IO_MD만 허용.
 * NVM/ZNS CSI인 경우 컨트롤러 기능(cdata->oncs, ctrlr_flags 등)에 따라 지원 여부 결정:
 *   UNMAP: oncs.nvmdsmsv, WRITE_ZEROES: oncs.nvmwzsv, COMPARE_AND_WRITE: CAW flag,
 *   ZONE_APPEND: ZNS+ZONE_APPEND flag, COPY: oncs.nvmcpys 등.
 * 컨텍스트: app 스레드 강제.
 *
 * 호출 체인:
 *   bdev 레이어(nvmelib_fn_table.io_type_supported) → [이 함수]
 */
static bool
bdev_nvme_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct nvme_bdev *nbdev = ctx;
	struct nvme_ns *nvme_ns;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_ctrlr *ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;

	assert(spdk_thread_is_app_thread(NULL));

	nvme_ns = TAILQ_FIRST(&nbdev->nvme_ns_list);
	assert(nvme_ns != NULL);
	ns = nvme_ns->ns;
	if (ns == NULL) {
		return false;
	}

	if (!bdev_nvme_is_supported_csi(spdk_nvme_ns_get_csi(ns))) {
		switch (io_type) {
		case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
		case SPDK_BDEV_IO_TYPE_NVME_IO:
			return true;

		case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
			return spdk_nvme_ns_get_md_size(ns) ? true : false;

		default:
			return false;
		}
	}

	ctrlr = spdk_nvme_ns_get_ctrlr(ns);

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_ABORT:
		return true;

	case SPDK_BDEV_IO_TYPE_NVME_NSSR:
		return spdk_nvme_ctrlr_is_nssr_supported(ctrlr);

	case SPDK_BDEV_IO_TYPE_COMPARE:
		return spdk_nvme_ns_supports_compare(ns);

	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return spdk_nvme_ns_get_md_size(ns) ? true : false;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		cdata = spdk_nvme_ctrlr_get_data(ctrlr);
		return cdata->oncs.nvmdsmsv;

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		cdata = spdk_nvme_ctrlr_get_data(ctrlr);
		return cdata->oncs.nvmwzsv;

	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
		if (spdk_nvme_ctrlr_get_flags(ctrlr) &
		    SPDK_NVME_CTRLR_COMPARE_AND_WRITE_SUPPORTED) {
			return true;
		}
		return false;

	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		return spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_ZNS;

	case SPDK_BDEV_IO_TYPE_ZONE_APPEND:
		return spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_ZNS &&
		       spdk_nvme_ctrlr_get_flags(ctrlr) & SPDK_NVME_CTRLR_ZONE_APPEND_SUPPORTED;

	case SPDK_BDEV_IO_TYPE_COPY:
		cdata = spdk_nvme_ctrlr_get_data(ctrlr);
		return cdata->oncs.nvmcpys;

	case SPDK_BDEV_IO_TYPE_WRITE_UNCORRECTABLE:
		return spdk_nvme_ns_supports_write_uncorrectable(ns);

	default:
		return false;
	}
}

/*
 * [한국어]
 * nvme_qpair_create - 컨트롤러 채널을 위한 nvme_qpair를 생성하고 poll group에 등록.
 *
 * @nvme_ctrlr: qpair를 생성할 컨트롤러
 * @ctrlr_ch: qpair를 연결할 컨트롤러 채널
 * @return: 0(성공), -ENOMEM(할당 실패), 기타 에러코드
 *
 * bdev_nvme_create_ctrlr_channel_cb()에서 io_device 채널 생성 시 호출됨.
 * nvme_qpair를 할당하고 spdk_get_io_channel(&g_nvme_bdev_ctrlrs)로 poll group 채널 획득.
 * VTUNE 빌드 시 collect_spin_stat=true 설정.
 * 컨트롤러가 disabled 상태가 아니면 bdev_nvme_create_qpair()로 실제 HW qpair 생성.
 * reconnect_delay_sec>0 AND bdev_retry_count>0이면 qpair 생성 실패를 무시(재시도 가능).
 * nvme_ctrlr 레퍼런스 카운트 증가.
 *
 * 호출 체인:
 *   bdev_nvme_create_ctrlr_channel_cb() → [이 함수]
 *   → bdev_nvme_create_qpair() → spdk_nvme_ctrlr_alloc_io_qpair()
 */
static int
nvme_qpair_create(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ctrlr_channel *ctrlr_ch)
{
	struct nvme_qpair *nvme_qpair;
	struct spdk_io_channel *pg_ch;
	int rc;

	nvme_qpair = calloc(1, sizeof(*nvme_qpair));
	if (!nvme_qpair) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "Failed to alloc nvme_qpair.\n");
		return -1;
	}

	TAILQ_INIT(&nvme_qpair->io_path_list);

	nvme_qpair->ctrlr = nvme_ctrlr;
	nvme_qpair->ctrlr_ch = ctrlr_ch;

	pg_ch = spdk_get_io_channel(&g_nvme_bdev_ctrlrs);
	if (!pg_ch) {
		free(nvme_qpair);
		return -1;
	}

	nvme_qpair->group = spdk_io_channel_get_ctx(pg_ch);

#ifdef SPDK_CONFIG_VTUNE
	nvme_qpair->group->collect_spin_stat = true;
#else
	nvme_qpair->group->collect_spin_stat = false;
#endif

	if (!nvme_ctrlr->disabled) {
		/* If a nvme_ctrlr is disabled, don't try to create qpair for it. Qpair will
		 * be created when it's enabled.
		 */
		rc = bdev_nvme_create_qpair(nvme_qpair);
		if (rc != 0) {
			/* nvme_ctrlr can't create IO qpair if connection is down.
			 * If reconnect_delay_sec is non-zero, creating IO qpair is retried
			 * after reconnect_delay_sec seconds. If bdev_retry_count is non-zero,
			 * submitted IO will be queued until IO qpair is successfully created.
			 *
			 * Hence, if both are satisfied, ignore the failure.
			 */
			if (nvme_ctrlr->opts.reconnect_delay_sec == 0 || g_opts.bdev_retry_count == 0) {
				spdk_put_io_channel(pg_ch);
				free(nvme_qpair);
				return rc;
			}
		}
	}

	TAILQ_INSERT_TAIL(&nvme_qpair->group->qpair_list, nvme_qpair, tailq);

	ctrlr_ch->qpair = nvme_qpair;

	nvme_ctrlr_get_ref(nvme_ctrlr);

	return 0;
}

/*
 * [한국어]
 * bdev_nvme_create_ctrlr_channel_cb - nvme_ctrlr io_device의 채널 생성 콜백.
 *
 * @io_device: nvme_ctrlr 포인터 (spdk_io_device_register 시 등록한 io_device)
 * @ctx_buf: 새로 할당된 nvme_ctrlr_channel 버퍼
 * @return: 0(성공), 에러코드(실패)
 *
 * spdk_get_io_channel(nvme_ctrlr) 호출 시 SPDK io_device 프레임워크가 이 콜백을 호출.
 * 실제 작업은 nvme_qpair_create()에 위임하여 nvme_qpair 할당 및 poll group 연결.
 * 컨텍스트: 해당 spdk_thread에서 실행.
 *
 * 호출 체인:
 *   spdk_get_io_channel(nvme_ctrlr) → SPDK io_device 프레임워크 → [이 함수] → nvme_qpair_create()
 */
static int
bdev_nvme_create_ctrlr_channel_cb(void *io_device, void *ctx_buf)
{
	struct nvme_ctrlr *nvme_ctrlr = io_device;
	struct nvme_ctrlr_channel *ctrlr_ch = ctx_buf;

	return nvme_qpair_create(nvme_ctrlr, ctrlr_ch);
}

/*
 * [한국어]
 * nvme_qpair_delete - nvme_qpair를 정리하고 메모리를 해제하는 내부 함수.
 *
 * @nvme_qpair: 삭제할 nvme_qpair 포인터
 * @return: void
 *
 * bdev_nvme_destroy_ctrlr_channel_cb()에서 qpair가 HW 연결 없이 삭제될 때 호출.
 * 1) io_path_list의 모든 io_path를 TAILQ_REMOVE 후 nvme_io_path_free()로 해제.
 * 2) qpair를 poll group의 qpair_list에서 제거.
 * 3) spdk_put_io_channel()로 poll group 채널 레퍼런스 반환.
 * 4) nvme_ctrlr_put_ref()로 컨트롤러 레퍼런스 감소.
 * 5) free(nvme_qpair)로 메모리 해제.
 * 컨텍스트: 해당 spdk_thread에서 실행.
 *
 * 호출 체인:
 *   bdev_nvme_destroy_ctrlr_channel_cb() → [이 함수]
 *   → nvme_io_path_free() / spdk_put_io_channel() / nvme_ctrlr_put_ref()
 */
static void
nvme_qpair_delete(struct nvme_qpair *nvme_qpair)
{
	struct nvme_io_path *io_path, *next;

	assert(nvme_qpair->group != NULL);

	TAILQ_FOREACH_SAFE(io_path, &nvme_qpair->io_path_list, tailq, next) {
		TAILQ_REMOVE(&nvme_qpair->io_path_list, io_path, tailq);
		nvme_io_path_free(io_path);
	}

	TAILQ_REMOVE(&nvme_qpair->group->qpair_list, nvme_qpair, tailq);

	spdk_put_io_channel(spdk_io_channel_from_ctx(nvme_qpair->group));

	nvme_ctrlr_put_ref(nvme_qpair->ctrlr);

	free(nvme_qpair);
}

/*
 * [한국어]
 * bdev_nvme_destroy_ctrlr_channel_cb - nvme_ctrlr io_device의 채널 삭제 콜백.
 *
 * @io_device: nvme_ctrlr 포인터
 * @ctx_buf: 삭제할 nvme_ctrlr_channel 버퍼
 * @return: void
 *
 * spdk_put_io_channel(ctrlr_ch) 시 SPDK io_device 프레임워크가 이 콜백을 호출.
 * I/O path 캐시를 클리어한 후 qpair 상태에 따라:
 *   - qpair가 HW 연결됨: disconnect 요청 후 비동기 해제를 위해 ctrlr_ch를 NULL로 분리.
 *   - qpair가 없음: nvme_qpair_delete()로 즉시 해제.
 * reset_iter가 활성이면 nvme_ctrlr_channel_reset_finish()로 리셋 완료 처리.
 * 컨텍스트: 해당 spdk_thread에서 실행.
 *
 * 호출 체인:
 *   spdk_put_io_channel(ctrlr_ch) → SPDK 프레임워크 → [이 함수]
 *   → spdk_nvme_ctrlr_disconnect_io_qpair() or nvme_qpair_delete()
 */
static void
bdev_nvme_destroy_ctrlr_channel_cb(void *io_device, void *ctx_buf)
{
	struct nvme_ctrlr_channel *ctrlr_ch = ctx_buf;
	struct nvme_qpair *nvme_qpair;

	nvme_qpair = ctrlr_ch->qpair;
	assert(nvme_qpair != NULL);

	_bdev_nvme_clear_io_path_cache(nvme_qpair);

	if (nvme_qpair->qpair != NULL) {
		/* Always try to disconnect the qpair, even if a reset is in progress.
		 * The qpair may have been created after the reset process started.
		 */
		spdk_nvme_ctrlr_disconnect_io_qpair(nvme_qpair->qpair);

		/* Reset may still be in progress on this channel; finish it before deleting the channel. */
		if (ctrlr_ch->reset_iter) {
			nvme_ctrlr_channel_reset_finish(ctrlr_ch, 0);
		}

		/* We cannot release a reference to the poll group now.
		 * The qpair may be disconnected asynchronously later.
		 * We need to poll it until it is actually disconnected.
		 * Just detach the qpair from the deleting ctrlr_channel.
		 */
		nvme_qpair->ctrlr_ch = NULL;
	} else {
		assert(ctrlr_ch->reset_iter == NULL);

		nvme_qpair_delete(nvme_qpair);
	}
}

/*
 * [한국어]
 * bdev_nvme_get_accel_channel - poll group의 accel I/O 채널을 지연 초기화하여 반환.
 *
 * @group: nvme_poll_group 포인터
 * @return: accel I/O 채널 포인터, 실패 시 NULL
 *
 * bdev_nvme_append_crc32c()와 bdev_nvme_append_copy()에서 호출.
 * accel_channel이 아직 없으면 spdk_accel_get_io_channel()로 획득 후 group에 캐시.
 * 이미 있으면 즉시 반환(빠른 경로). 지연 초기화 패턴으로 필요 시에만 채널 생성.
 * 컨텍스트: poll group 소속 spdk_thread.
 *
 * 호출 체인:
 *   bdev_nvme_append_crc32c() → [이 함수] → spdk_accel_get_io_channel()
 *   bdev_nvme_append_copy() → [이 함수] → spdk_accel_get_io_channel()
 */
static inline struct spdk_io_channel *
bdev_nvme_get_accel_channel(struct nvme_poll_group *group)
{
	if (spdk_unlikely(!group->accel_channel)) {
		group->accel_channel = spdk_accel_get_io_channel();
		if (!group->accel_channel) {
			SPDK_ERRLOG("Cannot get the accel_channel for bdev nvme polling group=%p\n",
				    group);
			return NULL;
		}
	}

	return group->accel_channel;
}

/*
 * [한국어]
 * bdev_nvme_finish_sequence - accel 시퀀스를 완료 처리하는 래퍼 함수.
 *
 * @seq: 완료할 accel 시퀀스 핸들
 * @cb_fn: 완료 콜백 (spdk_nvme_accel_completion_cb 타입)
 * @cb_arg: 콜백 인수
 * @return: void
 *
 * g_bdev_nvme_accel_fn_table.finish_sequence로 등록된 함수.
 * NVMe 드라이버가 accel 시퀀스 완료를 요청할 때 이 함수를 통해 spdk_accel_sequence_finish()를 호출.
 * 인터페이스 분리를 위한 래퍼 패턴.
 *
 * 호출 체인:
 *   spdk_nvme 드라이버(accel_fn_table.finish_sequence) → [이 함수] → spdk_accel_sequence_finish()
 */
static void
bdev_nvme_finish_sequence(void *seq, spdk_nvme_accel_completion_cb cb_fn, void *cb_arg)
{
	spdk_accel_sequence_finish(seq, cb_fn, cb_arg);
}

/*
 * [한국어]
 * bdev_nvme_abort_sequence - accel 시퀀스를 중단하는 래퍼 함수.
 *
 * @seq: 중단할 accel 시퀀스 핸들
 * @return: void
 *
 * g_bdev_nvme_accel_fn_table.abort_sequence로 등록된 함수.
 * NVMe 드라이버가 오류 발생 시 accel 시퀀스를 취소할 때 호출.
 * spdk_accel_sequence_abort()로 즉시 중단 처리.
 *
 * 호출 체인:
 *   spdk_nvme 드라이버(accel_fn_table.abort_sequence) → [이 함수] → spdk_accel_sequence_abort()
 */
static void
bdev_nvme_abort_sequence(void *seq)
{
	spdk_accel_sequence_abort(seq);
}

/*
 * [한국어]
 * bdev_nvme_reverse_sequence - accel 시퀀스 순서를 역전시키는 래퍼 함수.
 *
 * @seq: 역전할 accel 시퀀스 핸들
 * @return: void
 *
 * g_bdev_nvme_accel_fn_table.reverse_sequence로 등록된 함수.
 * 읽기 I/O에서 PI(Protection Information) 검증을 역순으로 처리할 때 호출.
 * spdk_accel_sequence_reverse()로 시퀀스 단계 순서를 반전.
 *
 * 호출 체인:
 *   spdk_nvme 드라이버(accel_fn_table.reverse_sequence) → [이 함수] → spdk_accel_sequence_reverse()
 */
static void
bdev_nvme_reverse_sequence(void *seq)
{
	spdk_accel_sequence_reverse(seq);
}

/*
 * [한국어]
 * bdev_nvme_append_crc32c - accel 시퀀스에 CRC32C 계산 단계를 추가하는 래퍼.
 *
 * @ctx: nvme_poll_group 포인터 (accel 채널 획득에 사용)
 * @seq: accel 시퀀스 핸들 포인터 (spdk_accel_sequence**)
 * @dst: CRC32C 결과를 저장할 uint32_t 포인터
 * @iovs: 입력 데이터 iovec 배열
 * @iovcnt: iovec 개수
 * @domain: 메모리 도메인 (DMA 가능 영역)
 * @domain_ctx: 메모리 도메인 컨텍스트
 * @seed: CRC32C 초기값
 * @cb_fn: 단계 완료 콜백
 * @cb_arg: 콜백 인수
 * @return: 0(성공), -ENOMEM(accel 채널 획득 실패), 기타 에러코드
 *
 * g_bdev_nvme_accel_fn_table.append_crc32c로 등록된 함수.
 * NVMe 드라이버가 PI(Protection Information) T10 DIF CRC 계산을 accel에 오프로드할 때 호출.
 * accel 채널을 지연 초기화 후 spdk_accel_append_crc32c()로 시퀀스에 단계 추가.
 *
 * 호출 체인:
 *   spdk_nvme 드라이버(accel_fn_table.append_crc32c) → [이 함수]
 *   → bdev_nvme_get_accel_channel() → spdk_accel_append_crc32c()
 */
static int
bdev_nvme_append_crc32c(void *ctx, void **seq, uint32_t *dst, struct iovec *iovs, uint32_t iovcnt,
			struct spdk_memory_domain *domain, void *domain_ctx, uint32_t seed,
			spdk_nvme_accel_step_cb cb_fn, void *cb_arg)
{
	struct spdk_io_channel *ch;
	struct nvme_poll_group *group = ctx;

	ch = bdev_nvme_get_accel_channel(group);
	if (spdk_unlikely(ch == NULL)) {
		return -ENOMEM;
	}

	return spdk_accel_append_crc32c((struct spdk_accel_sequence **)seq, ch, dst, iovs, iovcnt,
					domain, domain_ctx, seed, cb_fn, cb_arg);
}

/*
 * [한국어]
 * bdev_nvme_append_copy - accel 시퀀스에 메모리 복사 단계를 추가하는 래퍼.
 *
 * @ctx: nvme_poll_group 포인터
 * @seq: accel 시퀀스 핸들 포인터
 * @dst_iovs: 대상 iovec 배열
 * @dst_iovcnt: 대상 iovec 개수
 * @dst_domain: 대상 메모리 도메인
 * @dst_domain_ctx: 대상 메모리 도메인 컨텍스트
 * @src_iovs: 소스 iovec 배열
 * @src_iovcnt: 소스 iovec 개수
 * @src_domain: 소스 메모리 도메인
 * @src_domain_ctx: 소스 메모리 도메인 컨텍스트
 * @cb_fn: 단계 완료 콜백
 * @cb_arg: 콜백 인수
 * @return: 0(성공), -ENOMEM(accel 채널 획득 실패), 기타 에러코드
 *
 * g_bdev_nvme_accel_fn_table.append_copy로 등록된 함수.
 * NVMe 드라이버가 메모리 도메인 간 데이터 복사(예: DMA 전송)를 accel에 오프로드할 때 호출.
 * accel 채널을 지연 초기화 후 spdk_accel_append_copy()로 시퀀스에 단계 추가.
 *
 * 호출 체인:
 *   spdk_nvme 드라이버(accel_fn_table.append_copy) → [이 함수]
 *   → bdev_nvme_get_accel_channel() → spdk_accel_append_copy()
 */
static int
bdev_nvme_append_copy(void *ctx, void **seq, struct iovec *dst_iovs, uint32_t dst_iovcnt,
		      struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
		      struct iovec *src_iovs, uint32_t src_iovcnt,
		      struct spdk_memory_domain *src_domain, void *src_domain_ctx,
		      spdk_nvme_accel_step_cb cb_fn, void *cb_arg)
{
	struct spdk_io_channel *ch;
	struct nvme_poll_group *group = ctx;

	ch = bdev_nvme_get_accel_channel(group);
	if (spdk_unlikely(ch == NULL)) {
		return -ENOMEM;
	}

	return spdk_accel_append_copy((struct spdk_accel_sequence **)seq, ch,
				      dst_iovs, dst_iovcnt, dst_domain, dst_domain_ctx,
				      src_iovs, src_iovcnt, src_domain, src_domain_ctx,
				      cb_fn, cb_arg);
}

static struct spdk_nvme_accel_fn_table g_bdev_nvme_accel_fn_table = {
	.table_size		= sizeof(struct spdk_nvme_accel_fn_table),
	.append_crc32c		= bdev_nvme_append_crc32c,
	.append_copy		= bdev_nvme_append_copy,
	.finish_sequence	= bdev_nvme_finish_sequence,
	.reverse_sequence	= bdev_nvme_reverse_sequence,
	.abort_sequence		= bdev_nvme_abort_sequence,
};

/*
 * [한국어]
 * bdev_nvme_poll_group_interrupt_cb - interrupt 모드에서 poll group 인터럽트 발생 시 콜백.
 *
 * @group: NVMe poll group 핸들 (사용하지 않음 — ctx에서 group을 가져옴)
 * @ctx: nvme_poll_group 포인터
 * @return: void
 *
 * spdk_interrupt_mode_is_enabled()가 true일 때 인터럽트 기반 I/O 완료 알림이 도착하면 호출.
 * 실제 완료 처리는 bdev_nvme_poll()에 위임.
 * 인터럽트 모드에서 poller 대신 fd_group 인터럽트를 사용하는 설계를 지원.
 *
 * 호출 체인:
 *   spdk_nvme poll group 인터럽트 → [이 함수] → bdev_nvme_poll()
 */
static void
bdev_nvme_poll_group_interrupt_cb(struct spdk_nvme_poll_group *group, void *ctx)
{
	bdev_nvme_poll(ctx);
}

/*
 * [한국어]
 * bdev_nvme_create_poll_group_cb - g_nvme_bdev_ctrlrs io_device의 poll group 채널 생성 콜백.
 *
 * @io_device: g_nvme_bdev_ctrlrs 포인터 (전역 컨트롤러 목록)
 * @ctx_buf: 새로 할당된 nvme_poll_group 버퍼
 * @return: 0(성공), -1(실패)
 *
 * spdk_get_io_channel(&g_nvme_bdev_ctrlrs) 호출 시 SPDK io_device 프레임워크가 이 콜백을 호출.
 * 1) spdk_nvme_poll_group_create()로 NVMe poll group 생성 (accel fn_table 연결).
 * 2) interrupt 모드이면 period=0, polling 모드이면 g_opts.nvme_ioq_poll_period_us로 poller 등록.
 * 3) interrupt 모드이면 fd_group 기반 인터럽트 핸들러를 별도 등록.
 * 모든 qpair는 이 poll group을 통해 CQ 폴링/완료 처리됨.
 *
 * 호출 체인:
 *   nvme_qpair_create() → spdk_get_io_channel(&g_nvme_bdev_ctrlrs)
 *   → SPDK 프레임워크 → [이 함수] → spdk_nvme_poll_group_create()
 */
static int
bdev_nvme_create_poll_group_cb(void *io_device, void *ctx_buf)
{
	struct nvme_poll_group *group = ctx_buf;
	struct spdk_fd_group *fgrp;
	uint64_t period;
	int rc;

	TAILQ_INIT(&group->qpair_list);

	group->group = spdk_nvme_poll_group_create(group, &g_bdev_nvme_accel_fn_table);
	if (group->group == NULL) {
		return -1;
	}

	period = spdk_interrupt_mode_is_enabled() ? 0 : g_opts.nvme_ioq_poll_period_us;
	group->poller = SPDK_POLLER_REGISTER(bdev_nvme_poll, group, period);

	if (group->poller == NULL) {
		spdk_nvme_poll_group_destroy(group->group);
		return -1;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		spdk_poller_register_interrupt(group->poller, NULL, NULL);

		fgrp = spdk_nvme_poll_group_get_fd_group(group->group);
		if (fgrp == NULL) {
			spdk_nvme_poll_group_destroy(group->group);
			return -1;
		}

		rc = spdk_nvme_poll_group_set_interrupt_callback(group->group,
				bdev_nvme_poll_group_interrupt_cb, group);
		if (rc != 0) {
			spdk_nvme_poll_group_destroy(group->group);
			return -1;
		}

		group->intr = spdk_interrupt_register_fd_group(fgrp, "bdev_nvme_interrupt");
		if (!group->intr) {
			spdk_nvme_poll_group_destroy(group->group);
			return -1;
		}
	}

	return 0;
}

/*
 * [한국어]
 * bdev_nvme_destroy_poll_group_cb - g_nvme_bdev_ctrlrs io_device의 poll group 채널 삭제 콜백.
 *
 * @io_device: g_nvme_bdev_ctrlrs 포인터
 * @ctx_buf: 삭제할 nvme_poll_group 버퍼
 * @return: void
 *
 * spdk_put_io_channel(pg_ch) 시 SPDK io_device 프레임워크가 이 콜백을 호출.
 * qpair_list가 비어 있어야 함(assert).
 * accel_channel이 있으면 spdk_put_io_channel()로 반환.
 * interrupt 모드이면 spdk_interrupt_unregister()로 인터럽트 핸들러 해제.
 * poller 해제 후 spdk_nvme_poll_group_destroy()로 poll group 삭제.
 * 컨텍스트: 해당 spdk_thread.
 *
 * 호출 체인:
 *   nvme_qpair_delete() → spdk_put_io_channel(pg_ch)
 *   → SPDK 프레임워크 → [이 함수] → spdk_nvme_poll_group_destroy()
 */
static void
bdev_nvme_destroy_poll_group_cb(void *io_device, void *ctx_buf)
{
	struct nvme_poll_group *group = ctx_buf;

	assert(TAILQ_EMPTY(&group->qpair_list));

	if (group->accel_channel) {
		spdk_put_io_channel(group->accel_channel);
	}

	if (spdk_interrupt_mode_is_enabled()) {
		spdk_interrupt_unregister(&group->intr);
	}

	spdk_poller_unregister(&group->poller);
	if (spdk_nvme_poll_group_destroy(group->group)) {
		SPDK_ERRLOG("Unable to destroy a poll group for the NVMe bdev module.\n");
		assert(false);
	}
}

/*
 * [한국어]
 * bdev_nvme_get_io_channel - nvme_bdev의 I/O 채널(nvme_bdev_channel)을 획득하는 fn_table 콜백.
 *
 * @ctx: nvme_bdev 포인터 (bdev->ctxt)
 * @return: spdk_io_channel 포인터 (nvme_bdev_channel 포함)
 *
 * nvmelib_fn_table.get_io_channel로 등록된 함수.
 * bdev 레이어가 I/O 제출을 위해 채널을 요청할 때 호출.
 * nvme_bdev를 io_device로 spdk_get_io_channel()을 호출하여
 * bdev_nvme_create_bdev_channel_cb()가 실행되고 nvme_bdev_channel이 초기화됨.
 *
 * 호출 체인:
 *   bdev 레이어(fn_table.get_io_channel) → [이 함수]
 *   → spdk_get_io_channel(nbdev) → bdev_nvme_create_bdev_channel_cb()
 */
static struct spdk_io_channel *
bdev_nvme_get_io_channel(void *ctx)
{
	struct nvme_bdev *nbdev = ctx;

	return spdk_get_io_channel(nbdev);
}

/*
 * [한국어]
 * bdev_nvme_get_module_ctx - nvme_bdev의 모듈 컨텍스트(spdk_nvme_ns)를 반환.
 *
 * @ctx: nvme_bdev 포인터 (bdev->ctxt)
 * @return: spdk_nvme_ns 포인터(성공), NULL(bdev가 nvme_if 모듈이 아니거나 ns가 없음)
 *
 * nvmelib_fn_table.get_module_ctx로 등록된 함수.
 * bdev 레이어가 모듈 전용 데이터를 가져올 때 호출 (예: RPC 조회, PI 정보 조회).
 * nvme_ns_list의 첫 번째 ns의 spdk_nvme_ns를 반환.
 * 컨텍스트: app 스레드(assert 포함).
 *
 * 호출 체인:
 *   bdev 레이어(fn_table.get_module_ctx) → [이 함수]
 */
static void *
bdev_nvme_get_module_ctx(void *ctx)
{
	struct nvme_bdev *nbdev = ctx;
	struct nvme_ns *nvme_ns;

	assert(spdk_thread_is_app_thread(NULL));

	if (!nbdev || nbdev->disk.module != &nvme_if) {
		return NULL;
	}

	nvme_ns = TAILQ_FIRST(&nbdev->nvme_ns_list);
	if (!nvme_ns) {
		return NULL;
	}

	return nvme_ns->ns;
}

/*
 * [한국어]
 * _nvme_ana_state_str - ANA 상태 enum 값을 사람이 읽을 수 있는 문자열로 변환.
 *
 * @ana_state: spdk_nvme_ana_state enum 값
 * @return: ANA 상태 문자열 (예: "optimized", "non_optimized", "inaccessible" 등)
 *
 * bdev_nvme_dump_info_json() 및 nvme_namespace_info_json()에서 JSON 출력 시 호출.
 * NVMe-oF ANA(Asymmetric Namespace Access) 상태를 문자열로 변환.
 * 알 수 없는 상태는 "unknown"을 반환.
 *
 * 호출 체인:
 *   bdev_nvme_dump_info_json() → [이 함수]
 *   nvme_namespace_info_json() → [이 함수]
 */
static const char *
_nvme_ana_state_str(enum spdk_nvme_ana_state ana_state)
{
	switch (ana_state) {
	case SPDK_NVME_ANA_OPTIMIZED_STATE:
		return "optimized";
	case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
		return "non_optimized";
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
		return "inaccessible";
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		return "persistent_loss";
	case SPDK_NVME_ANA_CHANGE_STATE:
		return "change";
	default:
		return NULL;
	}
}

/*
 * [한국어]
 * bdev_nvme_get_memory_domains - 이 bdev가 지원하는 메모리 도메인 목록을 반환.
 *
 * @ctx: nvme_bdev 포인터 (bdev->ctxt)
 * @domains: 도메인을 저장할 배열 (NULL이면 카운트만 반환)
 * @array_size: 배열 크기
 * @return: 지원하는 메모리 도메인 수 (>0), 에러(< 0)
 *
 * nvmelib_fn_table.get_memory_domains로 등록된 함수.
 * bdev 레이어가 DMA 지원 메모리 도메인 목록을 쿼리할 때 호출.
 * nvme_ns_list의 모든 컨트롤러에 대해 spdk_nvme_ctrlr_get_memory_domains()를 호출하여 합산.
 * 컨텍스트: app 스레드(assert 포함).
 *
 * 호출 체인:
 *   bdev 레이어(fn_table.get_memory_domains) → [이 함수]
 *   → spdk_nvme_ctrlr_get_memory_domains()
 */
static int
bdev_nvme_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct spdk_memory_domain **_domains = NULL;
	struct nvme_bdev *nbdev = ctx;
	struct nvme_ns *nvme_ns;
	int i = 0, _array_size = array_size;
	int rc = 0;

	assert(spdk_thread_is_app_thread(NULL));

	TAILQ_FOREACH(nvme_ns, &nbdev->nvme_ns_list, tailq) {
		if (domains && array_size >= i) {
			_domains = &domains[i];
		} else {
			_domains = NULL;
		}
		rc = spdk_nvme_ctrlr_get_memory_domains(nvme_ns->ctrlr->ctrlr, _domains, _array_size);
		if (rc > 0) {
			i += rc;
			if (_array_size >= rc) {
				_array_size -= rc;
			} else {
				_array_size = 0;
			}
		} else if (rc < 0) {
			return rc;
		}
	}

	return i;
}

/*
 * [한국어]
 * nvme_ctrlr_get_state_str - nvme_ctrlr의 현재 상태를 사람이 읽을 수 있는 문자열로 반환.
 *
 * @nvme_ctrlr: 상태를 조회할 nvme_ctrlr 포인터
 * @return: 상태 문자열 (예: "deleting", "failed", "resetting", "reconnect_is_delayed",
 *          "disabled", "enabled")
 *
 * nvme_ctrlr_info_json()에서 JSON 출력 시 호출.
 * 우선순위: destruct > failed > resetting > reconnect_is_delayed > disabled > enabled.
 * 컨텍스트: app 스레드 (ctrlr 상태 필드 접근 시 mutex 보호 불필요 — app 스레드에서만).
 *
 * 호출 체인:
 *   nvme_ctrlr_info_json() → [이 함수]
 */
static const char *
nvme_ctrlr_get_state_str(struct nvme_ctrlr *nvme_ctrlr)
{
	if (nvme_ctrlr->destruct) {
		return "deleting";
	} else if (spdk_nvme_ctrlr_is_failed(nvme_ctrlr->ctrlr)) {
		return "failed";
	} else if (nvme_ctrlr->resetting) {
		return "resetting";
	} else if (nvme_ctrlr->reconnect_is_delayed > 0) {
		return "reconnect_is_delayed";
	} else if (nvme_ctrlr->disabled) {
		return "disabled";
	} else {
		return "enabled";
	}
}

/*
 * [한국어]
 * nvme_ctrlr_info_json - bdev_nvme.h §2 참조. nvme_ctrlr 정보를 JSON 객체로 직렬화.
 *
 * 출력 필드: state(deleting/failed/resetting/disabled/enabled), cuse_device(빌드 옵션 시),
 * trid (모든 path_id), hostnqn, numa_id, cntlid 등.
 * RPC bdev_nvme_get_controllers의 응답 빌드에서 호출.
 */
void
nvme_ctrlr_info_json(struct spdk_json_write_ctx *w, struct nvme_ctrlr *nvme_ctrlr)
{
	struct spdk_nvme_transport_id *trid;
	const struct spdk_nvme_ctrlr_opts *opts;
	const struct spdk_nvme_ctrlr_data *cdata;
	struct spdk_nvme_path_id *path_id;
	int32_t numa_id;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "state", nvme_ctrlr_get_state_str(nvme_ctrlr));

#ifdef SPDK_CONFIG_NVME_CUSE
	size_t cuse_name_size = 128;
	char cuse_name[cuse_name_size];

	int rc = spdk_nvme_cuse_get_ctrlr_name(nvme_ctrlr->ctrlr, cuse_name, &cuse_name_size);
	if (rc == 0) {
		spdk_json_write_named_string(w, "cuse_device", cuse_name);
	}
#endif
	trid = &nvme_ctrlr->active_path_id->trid;
	spdk_json_write_named_object_begin(w, "trid");
	nvme_bdev_dump_trid_json(trid, w);
	spdk_json_write_object_end(w);

	path_id = TAILQ_NEXT(nvme_ctrlr->active_path_id, link);
	if (path_id != NULL) {
		spdk_json_write_named_array_begin(w, "alternate_trids");
		do {
			trid = &path_id->trid;
			spdk_json_write_object_begin(w);
			nvme_bdev_dump_trid_json(trid, w);
			spdk_json_write_object_end(w);

			path_id = TAILQ_NEXT(path_id, link);
		} while (path_id != NULL);
		spdk_json_write_array_end(w);
	}

	cdata = spdk_nvme_ctrlr_get_data(nvme_ctrlr->ctrlr);
	spdk_json_write_named_uint16(w, "cntlid", cdata->cntlid);

	opts = spdk_nvme_ctrlr_get_opts(nvme_ctrlr->ctrlr);
	spdk_json_write_named_object_begin(w, "host");
	spdk_json_write_named_string(w, "nqn", opts->hostnqn);
	spdk_json_write_named_string(w, "addr", opts->src_addr);
	spdk_json_write_named_string(w, "svcid", opts->src_svcid);
	spdk_json_write_object_end(w);

	numa_id = spdk_nvme_ctrlr_get_numa_id(nvme_ctrlr->ctrlr);
	if (numa_id != SPDK_ENV_NUMA_ID_ANY) {
		spdk_json_write_named_uint32(w, "numa_id", numa_id);
	}
	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * nvme_namespace_info_json - 단일 nvme_ns의 상세 정보를 JSON 객체로 직렬화.
 *
 * @w: JSON 쓰기 컨텍스트
 * @nvme_ns: 정보를 직렬화할 nvme_ns 포인터
 * @return: void
 *
 * bdev_nvme_dump_info_json()에서 nvme_ns_list를 순회하며 호출.
 * ns가 NULL(아직 매핑 안 됨)이면 아무것도 쓰지 않고 반환.
 * 출력 필드:
 *   pci_address (PCIe인 경우), trid, ctrlr_data(cntlid, vendor_id, model_number,
 *   serial_number, firmware_revision), vs(NVMe 버전), ns_data(id, sector_size,
 *   md_size, pi_type, extended_lba_size, sectors), ana_state.
 *
 * 호출 체인:
 *   bdev_nvme_dump_info_json() → [이 함수]
 */
static void
nvme_namespace_info_json(struct spdk_json_write_ctx *w,
			 struct nvme_ns *nvme_ns)
{
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_ctrlr *ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;
	const struct spdk_nvme_transport_id *trid;
	union spdk_nvme_vs_register vs;
	const struct spdk_nvme_ns_data *nsdata;
	char buf[128];

	ns = nvme_ns->ns;
	if (ns == NULL) {
		return;
	}

	ctrlr = spdk_nvme_ns_get_ctrlr(ns);

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);
	vs = spdk_nvme_ctrlr_get_regs_vs(ctrlr);

	spdk_json_write_object_begin(w);

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		spdk_json_write_named_string(w, "pci_address", trid->traddr);
	}

	spdk_json_write_named_object_begin(w, "trid");

	nvme_bdev_dump_trid_json(trid, w);

	spdk_json_write_object_end(w);

#ifdef SPDK_CONFIG_NVME_CUSE
	size_t cuse_name_size = 128;
	char cuse_name[cuse_name_size];

	int rc = spdk_nvme_cuse_get_ns_name(ctrlr, spdk_nvme_ns_get_id(ns),
					    cuse_name, &cuse_name_size);
	if (rc == 0) {
		spdk_json_write_named_string(w, "cuse_device", cuse_name);
	}
#endif

	spdk_json_write_named_object_begin(w, "ctrlr_data");

	spdk_json_write_named_uint16(w, "cntlid", cdata->cntlid);

	spdk_json_write_named_string_fmt(w, "vendor_id", "0x%04x", cdata->vid);

	snprintf(buf, sizeof(cdata->mn) + 1, "%s", cdata->mn);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "model_number", buf);

	snprintf(buf, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "serial_number", buf);

	snprintf(buf, sizeof(cdata->fr) + 1, "%s", cdata->fr);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "firmware_revision", buf);

	if (cdata->subnqn[0] != '\0') {
		spdk_json_write_named_string(w, "subnqn", cdata->subnqn);
	}

	spdk_json_write_named_object_begin(w, "oacs");

	spdk_json_write_named_uint32(w, "security", cdata->oacs.ssrs);
	spdk_json_write_named_uint32(w, "format", cdata->oacs.fnvms);
	spdk_json_write_named_uint32(w, "firmware", cdata->oacs.fwds);
	spdk_json_write_named_uint32(w, "ns_manage", cdata->oacs.nms);

	spdk_json_write_object_end(w);

	spdk_json_write_named_bool(w, "multi_ctrlr", cdata->cmic.mctrs);
	spdk_json_write_named_bool(w, "ana_reporting", cdata->cmic.anars);

	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "vs");

	spdk_json_write_name(w, "nvme_version");
	if (vs.bits.ter) {
		spdk_json_write_string_fmt(w, "%u.%u.%u", vs.bits.mjr, vs.bits.mnr, vs.bits.ter);
	} else {
		spdk_json_write_string_fmt(w, "%u.%u", vs.bits.mjr, vs.bits.mnr);
	}

	spdk_json_write_object_end(w);

	nsdata = spdk_nvme_ns_get_data(ns);

	spdk_json_write_named_object_begin(w, "ns_data");

	spdk_json_write_named_uint32(w, "id", spdk_nvme_ns_get_id(ns));

	if (cdata->cmic.anars) {
		spdk_json_write_named_string(w, "ana_state",
					     _nvme_ana_state_str(nvme_ns->ana_state));
	}

	spdk_json_write_named_bool(w, "can_share", nsdata->nmic.shrns);

	spdk_json_write_object_end(w);

	if (cdata->oacs.ssrs) {
		spdk_json_write_named_object_begin(w, "security");

		spdk_json_write_named_bool(w, "opal", nvme_ns->bdev->opal);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * nvme_bdev_get_mp_policy_str - 멀티패스 정책 enum을 문자열로 변환.
 *
 * @nbdev: nvme_bdev 포인터
 * @return: "active_passive" 또는 "active_active"
 *
 * bdev_nvme_dump_info_json()에서 JSON 출력 시 호출.
 * BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE: 하나의 경로만 활성화.
 * BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE: 모든 경로를 동시 사용.
 *
 * 호출 체인:
 *   bdev_nvme_dump_info_json() → [이 함수]
 */
static const char *
nvme_bdev_get_mp_policy_str(struct nvme_bdev *nbdev)
{
	switch (nbdev->mp_policy) {
	case BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE:
		return "active_passive";
	case BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE:
		return "active_active";
	default:
		assert(false);
		return "invalid";
	}
}

/*
 * [한국어]
 * nvme_bdev_get_mp_selector_str - 멀티패스 선택기 enum을 문자열로 변환.
 *
 * @nbdev: nvme_bdev 포인터
 * @return: "round_robin" 또는 "queue_depth"
 *
 * bdev_nvme_dump_info_json()에서 mp_policy가 ACTIVE_ACTIVE일 때만 호출.
 * BDEV_NVME_MP_SELECTOR_ROUND_ROBIN: 경로를 순환하며 사용.
 * BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH: 가장 큐가 짧은 경로 우선.
 *
 * 호출 체인:
 *   bdev_nvme_dump_info_json() → [이 함수]
 */
static const char *
nvme_bdev_get_mp_selector_str(struct nvme_bdev *nbdev)
{
	switch (nbdev->mp_selector) {
	case BDEV_NVME_MP_SELECTOR_ROUND_ROBIN:
		return "round_robin";
	case BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH:
		return "queue_depth";
	default:
		assert(false);
		return "invalid";
	}
}

/*
 * [한국어]
 * bdev_nvme_dump_info_json - 이 bdev의 상세 정보를 JSON으로 직렬화 (fn_table.dump_info_json).
 *
 * @ctx: nvme_bdev 포인터 (bdev->ctxt)
 * @w: JSON 쓰기 컨텍스트
 * @return: 항상 0
 *
 * nvmelib_fn_table.dump_info_json으로 등록된 함수.
 * bdev 레이어가 "nvme" 배열(nvme_ns_list 순회)과 mp_policy/selector/rr_min_io를 출력.
 * 각 ns에 대해 nvme_namespace_info_json()을 호출.
 * 컨텍스트: app 스레드(assert 포함).
 *
 * 호출 체인:
 *   bdev 레이어(fn_table.dump_info_json) → [이 함수]
 *   → nvme_namespace_info_json()
 */
static int
bdev_nvme_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct nvme_bdev *nbdev = ctx;
	struct nvme_ns *nvme_ns;

	assert(spdk_thread_is_app_thread(NULL));

	spdk_json_write_named_array_begin(w, "nvme");
	TAILQ_FOREACH(nvme_ns, &nbdev->nvme_ns_list, tailq) {
		nvme_namespace_info_json(w, nvme_ns);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_named_string(w, "mp_policy", nvme_bdev_get_mp_policy_str(nbdev));
	if (nbdev->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE) {
		spdk_json_write_named_string(w, "selector", nvme_bdev_get_mp_selector_str(nbdev));
		if (nbdev->mp_selector == BDEV_NVME_MP_SELECTOR_ROUND_ROBIN) {
			spdk_json_write_named_uint32(w, "rr_min_io", nbdev->rr_min_io);
		}
	}

	return 0;
}

/*
 * [한국어]
 * bdev_nvme_write_config_json - bdev 별 설정을 JSON으로 직렬화 (fn_table.write_config_json).
 *
 * @bdev: spdk_bdev 포인터
 * @w: JSON 쓰기 컨텍스트
 * @return: void
 *
 * nvmelib_fn_table.write_config_json으로 등록된 함수.
 * nvme bdev는 개별 bdev 단위의 설정이 없으므로 아무것도 출력하지 않는다.
 * 전체 컨트롤러/bdev 설정은 bdev_nvme_config_json()에서 처리.
 *
 * 호출 체인:
 *   bdev 레이어(fn_table.write_config_json) → [이 함수] (no-op)
 */
static void
bdev_nvme_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev needed */
}

/*
 * [한국어]
 * bdev_nvme_get_spin_time - 이 채널의 I/O 폴링 spin 시간(마이크로초)을 반환.
 *
 * @ch: spdk_io_channel (nvme_bdev_channel 포함)
 * @return: 마이크로초 단위 spin 시간 (VTUNE 통계 수집 시에만 유효)
 *
 * nvmelib_fn_table.get_spin_time으로 등록된 함수.
 * collect_spin_stat=true인 poll group에 대해 start_ticks~end_ticks 구간을 spin_ticks에 누적.
 * 누적값을 spdk_get_ticks_hz()로 마이크로초 단위로 변환 후 반환.
 * SPDK_CONFIG_VTUNE 빌드 옵션 시에만 collect_spin_stat=true가 설정됨.
 *
 * 호출 체인:
 *   bdev 레이어(fn_table.get_spin_time) → [이 함수]
 */
static uint64_t
bdev_nvme_get_spin_time(struct spdk_io_channel *ch)
{
	struct nvme_bdev_channel *nbdev_ch = spdk_io_channel_get_ctx(ch);
	struct nvme_io_path *io_path;
	struct nvme_poll_group *group;
	uint64_t spin_time = 0;

	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		group = io_path->qpair->group;

		if (!group || !group->collect_spin_stat) {
			continue;
		}

		if (group->end_ticks != 0) {
			group->spin_ticks += (group->end_ticks - group->start_ticks);
			group->end_ticks = 0;
		}

		spin_time += group->spin_ticks;
		group->start_ticks = 0;
		group->spin_ticks = 0;
	}

	return (spin_time * 1000000ULL) / spdk_get_ticks_hz();
}

/*
 * [한국어]
 * bdev_nvme_reset_device_stat - nvme_bdev의 에러 통계를 초기화.
 *
 * @ctx: nvme_bdev 포인터 (bdev->ctxt)
 * @return: void
 *
 * nvmelib_fn_table.reset_device_stat으로 등록된 함수.
 * err_stat이 NULL이면 (통계 수집 안 함) 아무것도 하지 않음.
 * mutex를 획득하고 memset으로 err_stat을 0으로 초기화한 뒤 mutex 해제.
 * 컨텍스트: 어느 스레드에서든 호출 가능(mutex로 보호).
 *
 * 호출 체인:
 *   bdev 레이어(fn_table.reset_device_stat) → [이 함수]
 */
static void
bdev_nvme_reset_device_stat(void *ctx)
{
	struct nvme_bdev *nbdev = ctx;

	if (nbdev->err_stat == NULL) {
		return;
	}

	pthread_mutex_lock(&nbdev->mutex);
	memset(nbdev->err_stat, 0, sizeof(struct nvme_error_stat));
	pthread_mutex_unlock(&nbdev->mutex);
}

/*
 * [한국어]
 * bdev_nvme_format_nvme_status - NVMe 상태 문자열을 JSON 키 형식(소문자_밑줄)으로 변환.
 *
 * @dst: 변환 결과를 저장할 버퍼 (256바이트 이상 필요)
 * @src: 원본 NVMe 상태 문자열 (spdk_nvme_cpl_get_status_string 반환값)
 * @return: void
 *
 * bdev_nvme_dump_device_stat_json()에서 JSON 키를 생성할 때 호출.
 * " - " → "_", "-" → "_", " " → "_" 순서로 치환 후 소문자로 변환.
 * 예: "Invalid Field in Command" → "invalid_field_in_command"
 * JSON string should be lowercases and underscore delimited string.
 *
 * 호출 체인:
 *   bdev_nvme_dump_device_stat_json() → [이 함수]
 */
/* JSON string should be lowercases and underscore delimited string. */
static void
bdev_nvme_format_nvme_status(char *dst, const char *src)
{
	char tmp[256];

	spdk_strcpy_replace(dst, 256, src, " - ", "_");
	spdk_strcpy_replace(tmp, 256, dst, "-", "_");
	spdk_strcpy_replace(dst, 256, tmp, " ", "_");
	spdk_strlwr(dst);
}

/*
 * [한국어]
 * bdev_nvme_dump_device_stat_json - nvme_bdev의 에러 통계를 JSON으로 직렬화.
 *
 * @ctx: nvme_bdev 포인터 (bdev->ctxt)
 * @w: JSON 쓰기 컨텍스트
 * @return: void
 *
 * nvmelib_fn_table.dump_device_stat_json으로 등록된 함수.
 * err_stat이 NULL이면 (통계 수집 안 함) 아무것도 출력하지 않음.
 * status_type[8]과 status[4][256] 배열을 순회하여 0이 아닌 항목만 JSON에 출력.
 * 각 상태 코드를 bdev_nvme_format_nvme_status()로 소문자_밑줄 형식으로 변환 후 키로 사용.
 * 출력 구조: nvme_error → { status_type: {...}, status_code: {...} }
 *
 * 호출 체인:
 *   bdev 레이어(fn_table.dump_device_stat_json) → [이 함수]
 *   → bdev_nvme_format_nvme_status()
 */
static void
bdev_nvme_dump_device_stat_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct nvme_bdev *nbdev = ctx;
	struct spdk_nvme_status status = {};
	uint16_t sct, sc;
	char status_json[256];
	const char *status_str;

	if (nbdev->err_stat == NULL) {
		return;
	}

	spdk_json_write_named_object_begin(w, "nvme_error");

	spdk_json_write_named_object_begin(w, "status_type");
	for (sct = 0; sct < 8; sct++) {
		if (nbdev->err_stat->status_type[sct] == 0) {
			continue;
		}
		status.sct = sct;

		status_str = spdk_nvme_cpl_get_status_type_string(&status);
		assert(status_str != NULL);
		bdev_nvme_format_nvme_status(status_json, status_str);

		spdk_json_write_named_uint32(w, status_json, nbdev->err_stat->status_type[sct]);
	}
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "status_code");
	for (sct = 0; sct < 4; sct++) {
		status.sct = sct;
		for (sc = 0; sc < 256; sc++) {
			if (nbdev->err_stat->status[sct][sc] == 0) {
				continue;
			}
			status.sc = sc;

			status_str = spdk_nvme_cpl_get_status_string(&status);
			assert(status_str != NULL);
			bdev_nvme_format_nvme_status(status_json, status_str);

			spdk_json_write_named_uint32(w, status_json, nbdev->err_stat->status[sct][sc]);
		}
	}
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * bdev_nvme_accel_sequence_supported - 이 bdev가 특정 I/O 타입에 accel 시퀀스를 지원하는지 확인.
 *
 * @ctx: nvme_bdev 포인터 (bdev->ctxt)
 * @type: 확인할 I/O 타입
 * @return: true이면 지원, false이면 미지원
 *
 * nvmelib_fn_table.accel_sequence_supported로 등록된 함수.
 * g_opts.allow_accel_sequence가 false이면 무조건 false.
 * READ/WRITE 타입만 지원 가능 (나머지는 false).
 * SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED 플래그로 컨트롤러 기능 확인.
 * 컨텍스트: app 스레드(assert 포함).
 *
 * 호출 체인:
 *   bdev 레이어(fn_table.accel_sequence_supported) → [이 함수]
 */
static bool
bdev_nvme_accel_sequence_supported(void *ctx, enum spdk_bdev_io_type type)
{
	struct nvme_bdev *nbdev = ctx;
	struct nvme_ns *nvme_ns;
	struct spdk_nvme_ctrlr *ctrlr;

	assert(spdk_thread_is_app_thread(NULL));

	if (!g_opts.allow_accel_sequence) {
		return false;
	}

	switch (type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_READ:
		break;
	default:
		return false;
	}

	nvme_ns = TAILQ_FIRST(&nbdev->nvme_ns_list);
	assert(nvme_ns != NULL);

	ctrlr = nvme_ns->ctrlr->ctrlr;
	assert(ctrlr != NULL);

	return spdk_nvme_ctrlr_get_flags(ctrlr) & SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED;
}

static const struct spdk_bdev_fn_table nvmelib_fn_table = {
	.destruct			= bdev_nvme_destruct,
	.submit_request			= bdev_nvme_submit_request_initial,
	.io_type_supported		= bdev_nvme_io_type_supported,
	.get_io_channel			= bdev_nvme_get_io_channel,
	.dump_info_json			= bdev_nvme_dump_info_json,
	.write_config_json		= bdev_nvme_write_config_json,
	.get_spin_time			= bdev_nvme_get_spin_time,
	.get_module_ctx			= bdev_nvme_get_module_ctx,
	.get_memory_domains		= bdev_nvme_get_memory_domains,
	.accel_sequence_supported	= bdev_nvme_accel_sequence_supported,
	.reset_device_stat		= bdev_nvme_reset_device_stat,
	.dump_device_stat_json		= bdev_nvme_dump_device_stat_json,
};

typedef int (*bdev_nvme_parse_ana_log_page_cb)(
	const struct spdk_nvme_ana_group_descriptor *desc, void *cb_arg);

/*
 * [한국어]
 * bdev_nvme_parse_ana_log_page - ANA 로그 페이지를 파싱하여 각 group descriptor에 콜백 호출.
 *
 * @nvme_ctrlr: ANA 로그 페이지를 보유한 nvme_ctrlr
 * @cb_fn: 각 ANA 그룹 descriptor를 처리할 콜백 함수
 * @cb_arg: 콜백 인수
 * @return: 0(정상 완료), -EINVAL(ana_log_page가 NULL), 콜백에서 반환한 양수(조기 종료)
 *
 * nvme_ctrlr_set_ana_states(), nvme_ns_set_ana_state() 콜백 체인에서 사용.
 * ANA 로그 페이지 버퍼를 순회하면서 각 spdk_nvme_ana_group_descriptor를 복사본에 memcpy 후
 * cb_fn을 호출한다 (복사본 사용: 원본 DMA 버퍼 직접 접근 회피).
 * cb_fn이 양수를 반환하면 조기 종료 (1 = 해당 ns 발견, -1 = 에러).
 * 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   nvme_ctrlr_set_ana_states() → [이 함수] → nvme_ns_set_ana_state()
 *   nvme_ns_set_ana_state() 내부 → _nvme_ns_set_ana_state()
 */
static int
bdev_nvme_parse_ana_log_page(struct nvme_ctrlr *nvme_ctrlr,
			     bdev_nvme_parse_ana_log_page_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_ana_group_descriptor *copied_desc;
	uint8_t *orig_desc;
	uint32_t i, desc_size, copy_len;
	int rc = 0;

	if (nvme_ctrlr->ana_log_page == NULL) {
		return -EINVAL;
	}

	copied_desc = nvme_ctrlr->copied_ana_desc;

	orig_desc = (uint8_t *)nvme_ctrlr->ana_log_page + sizeof(struct spdk_nvme_ana_page);
	copy_len = nvme_ctrlr->max_ana_log_page_size - sizeof(struct spdk_nvme_ana_page);

	for (i = 0; i < nvme_ctrlr->ana_log_page->num_ana_group_desc; i++) {
		memcpy(copied_desc, orig_desc, copy_len);

		rc = cb_fn(copied_desc, cb_arg);
		if (rc != 0) {
			break;
		}

		desc_size = sizeof(struct spdk_nvme_ana_group_descriptor) +
			    copied_desc->num_of_nsid * sizeof(uint32_t);
		orig_desc += desc_size;
		copy_len -= desc_size;
	}

	return rc;
}

/*
 * [한국어]
 * nvme_ns_ana_transition_timedout - ANA 상태 전환 타임아웃 poller 콜백.
 *
 * @ctx: nvme_ns 포인터
 * @return: SPDK_POLLER_BUSY
 *
 * _nvme_ns_set_ana_state()에서 INACCESSIBLE/CHANGE 상태 진입 시 ANATT(Ana Transition Time) 타이머로 등록.
 * ANATT(cdata->anatt 초) 이후에도 OPTIMIZED/NON_OPTIMIZED로 전환되지 않으면 이 poller가 호출.
 * anatt_timer를 해제하고 ana_transition_timedout=true 설정.
 * ana_transition_timedout=true이면 해당 경로는 사용 불가로 간주됨.
 * 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   SPDK_POLLER_REGISTER(ANATT 타이머) → [이 함수]
 *   _nvme_ns_set_ana_state() → SPDK_POLLER_REGISTER → [이 함수 등록]
 */
static int
nvme_ns_ana_transition_timedout(void *ctx)
{
	struct nvme_ns *nvme_ns = ctx;

	spdk_poller_unregister(&nvme_ns->anatt_timer);
	nvme_ns->ana_transition_timedout = true;

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * _nvme_ns_set_ana_state - nvme_ns의 ANA 상태를 descriptor에서 읽어 실제로 업데이트.
 *
 * @nvme_ns: ANA 상태를 업데이트할 nvme_ns
 * @desc: 읽어온 spdk_nvme_ana_group_descriptor
 * @return: void
 *
 * nvme_ns_set_ana_state()가 해당 ns의 descriptor를 찾으면 이 함수를 호출.
 * ana_group_id와 ana_state를 업데이트하고 ana_state_updating=false로 설정.
 * OPTIMIZED/NON_OPTIMIZED: anatt_timer 해제 및 ana_transition_timedout=false.
 * INACCESSIBLE/CHANGE: anatt_timer가 없으면 ANATT 타이머 등록.
 * 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   nvme_ns_set_ana_state() → [이 함수]
 *   → SPDK_POLLER_REGISTER(nvme_ns_ana_transition_timedout) [ANATT 타이머 등록]
 */
static void
_nvme_ns_set_ana_state(struct nvme_ns *nvme_ns,
		       const struct spdk_nvme_ana_group_descriptor *desc)
{
	const struct spdk_nvme_ctrlr_data *cdata;

	nvme_ns->ana_group_id = desc->ana_group_id;
	nvme_ns->ana_state = desc->ana_state;
	nvme_ns->ana_state_updating = false;

	switch (nvme_ns->ana_state) {
	case SPDK_NVME_ANA_OPTIMIZED_STATE:
	case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
		nvme_ns->ana_transition_timedout = false;
		spdk_poller_unregister(&nvme_ns->anatt_timer);
		break;

	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
	case SPDK_NVME_ANA_CHANGE_STATE:
		if (nvme_ns->anatt_timer != NULL) {
			break;
		}

		cdata = spdk_nvme_ctrlr_get_data(nvme_ns->ctrlr->ctrlr);
		nvme_ns->anatt_timer = SPDK_POLLER_REGISTER(nvme_ns_ana_transition_timedout,
				       nvme_ns,
				       cdata->anatt * SPDK_SEC_TO_USEC);
		break;
	default:
		break;
	}
}

/*
 * [한국어]
 * nvme_ns_set_ana_state - 단일 nvme_ns에 대한 ANA 그룹 descriptor를 매칭하고 상태 적용.
 *
 * @desc: 파싱된 ANA 그룹 descriptor
 * @cb_arg: nvme_ns 포인터
 * @return: 1(이 ns를 발견하여 처리 완료), 0(이 descriptor는 해당 ns와 무관)
 *
 * bdev_nvme_parse_ana_log_page()의 콜백으로 등록되어 각 ANA group descriptor에 대해 호출.
 * descriptor의 nsid 배열을 순회하여 nvme_ns의 ns id와 일치하는지 확인.
 * 일치하면 _nvme_ns_set_ana_state()로 실제 상태 업데이트 후 1을 반환(파싱 조기 종료).
 * 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   bdev_nvme_parse_ana_log_page() → [이 함수] → _nvme_ns_set_ana_state()
 */
static int
nvme_ns_set_ana_state(const struct spdk_nvme_ana_group_descriptor *desc, void *cb_arg)
{
	struct nvme_ns *nvme_ns = cb_arg;
	uint32_t i;

	assert(nvme_ns->ns != NULL);

	for (i = 0; i < desc->num_of_nsid; i++) {
		if (desc->nsid[i] != spdk_nvme_ns_get_id(nvme_ns->ns)) {
			continue;
		}

		_nvme_ns_set_ana_state(nvme_ns, desc);
		return 1;
	}

	return 0;
}

/*
 * [한국어]
 * nvme_generate_uuid - SN(Serial Number)과 NSID로 결정론적(deterministic) UUID 생성.
 *
 * @sn: 컨트롤러 시리얼 넘버 문자열 (최대 SPDK_NVME_CTRLR_SN_LEN)
 * @nsid: Namespace ID (uint32_t)
 * @uuid: 생성된 UUID를 저장할 spdk_uuid 포인터
 * @return: 0(성공), -EINVAL(문자열 포맷 실패), spdk_uuid_generate_sha1 에러코드
 *
 * nbdev_create()에서 NGUID와 UUID가 없고 g_opts.generate_uuids=true일 때 호출.
 * "SN + NSID" 문자열에 대해 SHA1 기반 UUID v5를 생성.
 * namespace_uuid = "edaed2de-24bc-4b07-b559-f47ecbe730fd" (고정 namespace UUID).
 * 동일 SN과 NSID에 대해 항상 동일한 UUID를 생성하므로 재시작 후에도 bdev 이름 연속성 유지.
 * 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   nbdev_create() → [이 함수] → spdk_uuid_generate_sha1()
 */
static int
nvme_generate_uuid(const char *sn, uint32_t nsid, struct spdk_uuid *uuid)
{
	int rc = 0;
	struct spdk_uuid new_uuid, namespace_uuid;
	char merged_str[SPDK_NVME_CTRLR_SN_LEN + NSID_STR_LEN + 1] = {'\0'};
	/* This namespace UUID was generated using uuid_generate() method. */
	const char *namespace_str = {"edaed2de-24bc-4b07-b559-f47ecbe730fd"};
	int size;

	assert(strlen(sn) <= SPDK_NVME_CTRLR_SN_LEN);

	spdk_uuid_set_null(&new_uuid);
	spdk_uuid_set_null(&namespace_uuid);

	size = snprintf(merged_str, sizeof(merged_str), "%s%"PRIu32, sn, nsid);
	if (size <= 0 || (unsigned long)size >= sizeof(merged_str)) {
		return -EINVAL;
	}

	spdk_uuid_parse(&namespace_uuid, namespace_str);

	rc = spdk_uuid_generate_sha1(&new_uuid, &namespace_uuid, merged_str, size);
	if (rc == 0) {
		memcpy(uuid, &new_uuid, sizeof(struct spdk_uuid));
	}

	return rc;
}

/*
 * [한국어]
 * nbdev_create - spdk_bdev 구조체를 NVMe namespace 정보로 초기화 (bdev 등록 준비).
 *
 * @disk: 초기화할 spdk_bdev 구조체 포인터
 * @base_name: 컨트롤러 이름 (bdev 이름: "<base_name>n<nsid>" 형식)
 * @ctrlr: 컨트롤러 핸들
 * @ns: namespace 핸들
 * @bdev_opts: bdev 생성 옵션 (allow_unrecognized_csi 등)
 * @ctx: 미사용 (NULL)
 * @return: 0(성공), -ENOTSUP(지원하지 않는 CSI), -ENOMEM(이름 할당 실패), 기타 에러
 *
 * nvme_bdev_alloc()에서 호출. CSI(NVM/ZNS/기타)에 따라 bdev 속성 설정:
 *   UUID: NGUID > UUID > (generate_uuids=true이면 nvme_generate_uuid) 순서로 결정.
 *   blocklen, blockcnt, max_segment_size, max_num_segments, optimal_io_boundary 설정.
 *   atomic_write_unit(NAWUPF/NAWUN)과 phys_bs(NPWG) 설정.
 *   fn_table: nvmelib_fn_table, module: nvme_if.
 * 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   nvme_bdev_alloc() → [이 함수] → nvme_generate_uuid() (조건부)
 */
static int
nbdev_create(struct spdk_bdev *disk, const char *base_name,
	     struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns,
	     struct spdk_bdev_nvme_ctrlr_opts *bdev_opts, void *ctx)
{
	const struct spdk_uuid		*uuid;
	const uint8_t *nguid;
	const struct spdk_nvme_ctrlr_data *cdata;
	const struct spdk_nvme_ns_data	*nsdata;
	const struct spdk_nvme_ctrlr_opts *opts;
	enum spdk_nvme_csi		csi;
	uint32_t atomic_bs, phys_bs, bs;
	char sn_tmp[SPDK_NVME_CTRLR_SN_LEN + 1] = {'\0'};
	int rc;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	csi = spdk_nvme_ns_get_csi(ns);
	opts = spdk_nvme_ctrlr_get_opts(ctrlr);

	switch (csi) {
	case SPDK_NVME_CSI_NVM:
		disk->product_name = "NVMe disk";
		break;
	case SPDK_NVME_CSI_ZNS:
		disk->product_name = "NVMe ZNS disk";
		disk->zoned = true;
		disk->zone_size = spdk_nvme_zns_ns_get_zone_size_sectors(ns);
		disk->max_zone_append_size = spdk_nvme_zns_ctrlr_get_max_zone_append_size(ctrlr) /
					     spdk_nvme_ns_get_extended_sector_size(ns);
		disk->max_open_zones = spdk_nvme_zns_ns_get_max_open_zones(ns);
		disk->max_active_zones = spdk_nvme_zns_ns_get_max_active_zones(ns);
		break;
	default:
		if (bdev_opts->allow_unrecognized_csi) {
			disk->product_name = "NVMe Passthrough disk";
			break;
		}
		SPDK_ERRLOG("unsupported CSI: %u\n", csi);
		return -ENOTSUP;
	}

	nguid = spdk_nvme_ns_get_nguid(ns);
	if (!nguid) {
		uuid = spdk_nvme_ns_get_uuid(ns);
		if (uuid) {
			disk->uuid = *uuid;
		} else if (g_opts.generate_uuids) {
			spdk_strcpy_pad(sn_tmp, cdata->sn, SPDK_NVME_CTRLR_SN_LEN, '\0');
			rc = nvme_generate_uuid(sn_tmp, spdk_nvme_ns_get_id(ns), &disk->uuid);
			if (rc < 0) {
				SPDK_ERRLOG("UUID generation failed (%s)\n", spdk_strerror(-rc));
				return rc;
			}
		}
	} else {
		memcpy(&disk->uuid, nguid, sizeof(disk->uuid));
	}

	disk->name = spdk_sprintf_alloc("%sn%d", base_name, spdk_nvme_ns_get_id(ns));
	if (!disk->name) {
		return -ENOMEM;
	}

	disk->write_cache = 0;
	if (cdata->vwc.present) {
		/* Enable if the Volatile Write Cache exists */
		disk->write_cache = 1;
	}
	if (cdata->oncs.nvmwzsv) {
		disk->max_write_zeroes = UINT16_MAX + 1;
	}
	disk->blocklen = spdk_nvme_ns_get_extended_sector_size(ns);
	disk->blockcnt = spdk_nvme_ns_get_num_sectors(ns);
	disk->max_segment_size = spdk_nvme_ctrlr_get_max_xfer_size(ctrlr);
	disk->ctratt.raw = cdata->ctratt.raw;
	disk->nsid = spdk_nvme_ns_get_id(ns);
	/* NVMe driver will split one request into multiple requests
	 * based on MDTS and stripe boundary, the bdev layer will use
	 * max_segment_size and max_num_segments to split one big IO
	 * into multiple requests, then small request can't run out
	 * of NVMe internal requests data structure.
	 */
	if (opts && opts->io_queue_requests) {
		disk->max_num_segments = opts->io_queue_requests / 2;
	}
	if (spdk_nvme_ctrlr_get_flags(ctrlr) & SPDK_NVME_CTRLR_SGL_SUPPORTED) {
		/* The nvme driver will try to split I/O that have too many
		 * SGEs, but it doesn't work if that last SGE doesn't end on
		 * an aggregate total that is block aligned. The bdev layer has
		 * a more robust splitting framework, so use that instead for
		 * this case. (See issue #3269.)
		 */
		uint16_t max_sges = spdk_nvme_ctrlr_get_max_sges(ctrlr);

		if (disk->max_num_segments == 0) {
			disk->max_num_segments = max_sges;
		} else {
			disk->max_num_segments = spdk_min(disk->max_num_segments, max_sges);
		}
	}
	disk->optimal_io_boundary = spdk_nvme_ns_get_optimal_io_boundary(ns);

	nsdata = spdk_nvme_ns_get_data(ns);
	bs = spdk_nvme_ns_get_sector_size(ns);
	atomic_bs = bs;
	phys_bs = bs;
	if (nsdata->nabo == 0) {
		if (nsdata->nsfeat.ns_atomic_write_unit && nsdata->nawupf) {
			atomic_bs = bs * (1 + nsdata->nawupf);
		} else {
			atomic_bs = bs * (1 + cdata->awupf);
		}
	}
	if (nsdata->nsfeat.optperf) {
		phys_bs = bs * (1 + nsdata->npwg);

		disk->preferred_write_granularity = nsdata->npwg + 1;
		disk->preferred_write_alignment = nsdata->npwa + 1;
		disk->optimal_write_size = nsdata->nows + 1;
		disk->preferred_unmap_granularity = nsdata->npdg + 1;
		disk->preferred_unmap_alignment = nsdata->npda + 1;
	}
	disk->phys_blocklen = spdk_min(phys_bs, atomic_bs);

	disk->md_len = spdk_nvme_ns_get_md_size(ns);
	if (disk->md_len != 0) {
		disk->md_interleave = nsdata->flbas.extended;
		disk->dif_type = (enum spdk_dif_type)spdk_nvme_ns_get_pi_type(ns);
		if (disk->dif_type != SPDK_DIF_DISABLE) {
			disk->dif_is_head_of_md = nsdata->dps.md_start;
			disk->dif_check_flags = bdev_opts->prchk_flags;
			disk->dif_pi_format = (enum spdk_dif_pi_format)spdk_nvme_ns_get_pi_format(ns);
		}
	}

	if (!(spdk_nvme_ctrlr_get_flags(ctrlr) &
	      SPDK_NVME_CTRLR_COMPARE_AND_WRITE_SUPPORTED)) {
		disk->acwu = 0;
	} else if (nsdata->nsfeat.ns_atomic_write_unit) {
		disk->acwu = nsdata->nacwu + 1; /* 0-based */
	} else {
		disk->acwu = cdata->acwu + 1; /* 0-based */
	}

	if (cdata->oncs.nvmcpys) {
		/* For now bdev interface allows only single segment copy */
		disk->max_copy = nsdata->mssrl;
	}

	disk->ctxt = ctx;
	disk->fn_table = &nvmelib_fn_table;
	disk->module = &nvme_if;

	disk->numa.id_valid = 1;
	disk->numa.id = spdk_nvme_ctrlr_get_numa_id(ctrlr);

	return 0;
}

/*
 * [한국어]
 * nvme_bdev_alloc - nvme_bdev 구조체를 할당하고 기본값으로 초기화.
 *
 * @return: 초기화된 nvme_bdev 포인터(성공), NULL(실패)
 *
 * nvme_bdev_create()에서 호출. calloc으로 nvme_bdev를 할당하고:
 *   g_opts.nvme_error_stat=true이면 err_stat도 calloc으로 할당.
 *   pthread_mutex_init()으로 mutex 초기화.
 *   ref=1, mp_policy=ACTIVE_PASSIVE, mp_selector=ROUND_ROBIN, rr_min_io=UINT32_MAX 설정.
 *   nvme_ns_list TAILQ 초기화.
 * 실패 시 부분 할당을 해제하고 NULL 반환.
 * 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   nvme_bdev_create() → [이 함수]
 */
static struct nvme_bdev *
nvme_bdev_alloc(void)
{
	struct nvme_bdev *nbdev;
	int rc;

	nbdev = calloc(1, sizeof(*nbdev));
	if (!nbdev) {
		SPDK_ERRLOG("nbdev calloc() failed\n");
		return NULL;
	}

	if (g_opts.nvme_error_stat) {
		nbdev->err_stat = calloc(1, sizeof(struct nvme_error_stat));
		if (!nbdev->err_stat) {
			SPDK_ERRLOG("err_stat calloc() failed\n");
			free(nbdev);
			return NULL;
		}
	}

	rc = pthread_mutex_init(&nbdev->mutex, NULL);
	if (rc != 0) {
		free(nbdev->err_stat);
		free(nbdev);
		return NULL;
	}

	nbdev->ref = 1;
	nbdev->mp_policy = BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE;
	nbdev->mp_selector = BDEV_NVME_MP_SELECTOR_ROUND_ROBIN;
	nbdev->rr_min_io = UINT32_MAX;
	TAILQ_INIT(&nbdev->nvme_ns_list);

	return nbdev;
}

/*
 * [한국어]
 * nvme_bdev_create - 새 nvme_bdev를 생성하고 bdev 프레임워크에 등록.
 *
 * @nvme_ctrlr: bdev를 생성할 컨트롤러
 * @nvme_ns: bdev로 노출할 namespace
 * @return: 0(성공), -ENOMEM(할당 실패), nbdev_create 에러코드, spdk_bdev_register 에러코드
 *
 * nvme_ctrlr_populate_namespace()에서 호출.
 * 1) nvme_bdev_alloc()으로 nvme_bdev 할당.
 * 2) nbdev_create()으로 spdk_bdev 필드 초기화.
 * 3) spdk_io_device_register()로 nvme_bdev를 io_device로 등록 (채널 관리용).
 * 4) nvme_ns->bdev 연결 및 nbdev_ctrlr->bdevs에 추가.
 * 5) spdk_bdev_register()로 bdev 레이어에 등록 (상위 모듈에서 사용 가능해짐).
 * 실패 시 단계별 정리 후 에러 반환.
 * 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   nvme_ctrlr_populate_namespace() → [이 함수]
 *   → nvme_bdev_alloc() → nbdev_create() → spdk_bdev_register()
 */
static int
nvme_bdev_create(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *nvme_ns)
{
	struct nvme_bdev *nbdev;
	struct nvme_bdev_ctrlr *nbdev_ctrlr = nvme_ctrlr->nbdev_ctrlr;
	int rc;

	assert(spdk_thread_is_app_thread(NULL));

	nbdev = nvme_bdev_alloc();
	if (nbdev == NULL) {
		NVME_NS_ERRLOG(nvme_ns, "Failed to allocate NVMe bdev\n");
		return -ENOMEM;
	}

	nbdev->opal = nvme_ctrlr->opal_dev != NULL;

	rc = nbdev_create(&nbdev->disk, nbdev_ctrlr->name, nvme_ctrlr->ctrlr,
			  nvme_ns->ns, &nvme_ctrlr->opts, nbdev);
	if (rc != 0) {
		NVME_NS_ERRLOG(nvme_ns, "Failed to create NVMe disk\n");
		nvme_bdev_free(nbdev);
		return rc;
	}

	spdk_io_device_register(nbdev,
				bdev_nvme_create_bdev_channel_cb,
				bdev_nvme_destroy_bdev_channel_cb,
				sizeof(struct nvme_bdev_channel),
				nbdev->disk.name);

	nvme_ns->bdev = nbdev;
	nbdev->nsid = nvme_ns->id;
	TAILQ_INSERT_TAIL(&nbdev->nvme_ns_list, nvme_ns, tailq);

	nbdev->nbdev_ctrlr = nbdev_ctrlr;
	TAILQ_INSERT_TAIL(&nbdev_ctrlr->bdevs, nbdev, tailq);

	rc = spdk_bdev_register(&nbdev->disk);
	if (rc != 0) {
		NVME_NS_ERRLOG(nvme_ns, "spdk_bdev_register() failed\n");
		spdk_io_device_unregister(nbdev, NULL);
		nvme_ns->bdev = NULL;

		TAILQ_REMOVE(&nbdev_ctrlr->bdevs, nbdev, tailq);
		nvme_bdev_free(nbdev);
		return rc;
	}

	NVME_NS_DEBUGLOG(nvme_ns, "nbdev created\n");
	return 0;
}

/*
 * [한국어]
 * bdev_nvme_compare_ns - 두 namespace가 동일한 물리 namespace를 나타내는지 비교.
 *
 * @ns1: 첫 번째 namespace 핸들
 * @ns2: 두 번째 namespace 핸들
 * @return: true이면 동일 namespace, false이면 다른 namespace
 *
 * nvme_bdev_add_ns()에서 multipath 경로 추가 시 동일 namespace 여부를 확인하기 위해 호출.
 * 비교 항목: NGUID, EUI64, UUID(양쪽 다 NULL이거나 값이 같아야 함), CSI.
 * 컨텍스트: app 스레드.
 *
 * 호출 체인:
 *   nvme_bdev_add_ns() → [이 함수]
 */
static bool
bdev_nvme_compare_ns(struct spdk_nvme_ns *ns1, struct spdk_nvme_ns *ns2)
{
	const struct spdk_nvme_ns_data *nsdata1, *nsdata2;
	const struct spdk_uuid *uuid1, *uuid2;

	nsdata1 = spdk_nvme_ns_get_data(ns1);
	nsdata2 = spdk_nvme_ns_get_data(ns2);
	uuid1 = spdk_nvme_ns_get_uuid(ns1);
	uuid2 = spdk_nvme_ns_get_uuid(ns2);

	return memcmp(nsdata1->nguid, nsdata2->nguid, sizeof(nsdata1->nguid)) == 0 &&
	       nsdata1->eui64 == nsdata2->eui64 &&
	       ((uuid1 == NULL && uuid2 == NULL) ||
		(uuid1 != NULL && uuid2 != NULL && spdk_uuid_compare(uuid1, uuid2) == 0)) &&
	       spdk_nvme_ns_get_csi(ns1) == spdk_nvme_ns_get_csi(ns2);
}

/*
 * [한국어]
 * hotplug_probe_cb - hotplug 감지된 NVMe 장치를 attach할지 결정하는 콜백.
 *
 * @cb_ctx: 미사용 (NULL)
 * @trid: 감지된 장치의 transport ID
 * @opts: 연결 옵션 (이 함수에서 일부 필드를 설정함)
 * @return: true이면 attach, false이면 스킵
 *
 * bdev_nvme_hotplug()의 spdk_nvme_probe_poll_async() 콜백으로 등록.
 * g_skipped_nvme_ctrlrs 목록에 있는 장치는 스킵.
 * arbitration/priority 가중치와 disable_read_ana_log_page 옵션 설정.
 * 컨텍스트: app 스레드 (bdev_nvme_hotplug poller에서 호출).
 *
 * 호출 체인:
 *   spdk_nvme_probe_poll_async() → [이 함수]
 */
static bool
hotplug_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_probe_skip_entry *entry;

	TAILQ_FOREACH(entry, &g_skipped_nvme_ctrlrs, tailq) {
		if (spdk_nvme_transport_id_compare(trid, &entry->trid) == 0) {
			return false;
		}
	}

	opts->arbitration_burst = (uint8_t)g_opts.arbitration_burst;
	opts->low_priority_weight = (uint8_t)g_opts.low_priority_weight;
	opts->medium_priority_weight = (uint8_t)g_opts.medium_priority_weight;
	opts->high_priority_weight = (uint8_t)g_opts.high_priority_weight;
	opts->disable_read_ana_log_page = true;

	SPDK_DEBUGLOG(bdev_nvme, "Attaching to %s\n", trid->traddr);

	return true;
}

static void
nvme_abort_cpl(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_WARNLOG(nvme_ctrlr, "Abort failed. Resetting controller. sc is %u, sct is %u.\n",
				   cpl->status.sc, cpl->status.sct);
		bdev_nvme_reset_ctrlr(nvme_ctrlr);
	} else if (cpl->cdw0 & 0x1) {
		NVME_CTRLR_WARNLOG(nvme_ctrlr, "Specified command could not be aborted.\n");
		bdev_nvme_reset_ctrlr(nvme_ctrlr);
	}
}

static void
timeout_cb(void *cb_arg, struct spdk_nvme_ctrlr *ctrlr,
	   struct spdk_nvme_qpair *qpair, uint16_t cid)
{
	struct nvme_ctrlr *nvme_ctrlr = cb_arg;
	union spdk_nvme_csts_register csts;
	int rc;

	assert(nvme_ctrlr->ctrlr == ctrlr);

	NVME_CTRLR_WARNLOG(nvme_ctrlr, "Warning: Detected a timeout. ctrlr:%p,qpair:%p,cid:%u\n", ctrlr,
			   qpair, cid);

	/* Only try to read CSTS if it's a PCIe controller or we have a timeout on an I/O
	 * queue.  (Note: qpair == NULL when there's an admin cmd timeout.)  Otherwise we
	 * would submit another fabrics cmd on the admin queue to read CSTS and check for its
	 * completion recursively.
	 */
	if (nvme_ctrlr->active_path_id->trid.trtype == SPDK_NVME_TRANSPORT_PCIE || qpair != NULL) {
		csts = spdk_nvme_ctrlr_get_regs_csts(ctrlr);
		if (csts.bits.cfs) {
			NVME_CTRLR_ERRLOG(nvme_ctrlr, "%s on qpair:%p, reset required\n",
					  csts.raw == 0xFFFFFFFF ? "Could not read csts register" : "Controller Fatal Status", qpair);
			bdev_nvme_reset_ctrlr(nvme_ctrlr);
			return;
		}
	}

	switch (g_opts.action_on_timeout) {
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT:
		if (qpair) {
			/* Don't send abort to ctrlr when ctrlr is not available. */
			pthread_mutex_lock(&nvme_ctrlr->mutex);
			if (!nvme_ctrlr_is_available(nvme_ctrlr)) {
				pthread_mutex_unlock(&nvme_ctrlr->mutex);
				NVME_CTRLR_NOTICELOG(nvme_ctrlr, "Quit abort on qpair:%p. Ctrlr is not available.\n", qpair);
				return;
			}
			pthread_mutex_unlock(&nvme_ctrlr->mutex);

			rc = spdk_nvme_ctrlr_cmd_abort(ctrlr, qpair, cid,
						       nvme_abort_cpl, nvme_ctrlr);
			if (rc == 0) {
				return;
			}

			NVME_CTRLR_ERRLOG(nvme_ctrlr, "Unable to send abort on qpair:%p. Resetting, rc is %d.\n", qpair,
					  rc);
		}

	/* FALLTHROUGH */
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET:
		bdev_nvme_reset_ctrlr(nvme_ctrlr);
		break;
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE:
		NVME_CTRLR_DEBUGLOG(nvme_ctrlr, "No action for nvme controller timeout.\n");
		break;
	default:
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "An invalid timeout action value is found.\n");
		break;
	}
}

static struct nvme_ns *
nvme_ns_create(struct nvme_ctrlr *nvme_ctrlr, uint32_t nsid, struct nvme_async_probe_ctx *ctx)
{
	struct nvme_ns *nvme_ns;
	struct spdk_nvme_ns *ns;

	nvme_ns = calloc(1, sizeof(struct nvme_ns));
	if (nvme_ns == NULL) {
		return NULL;
	}

	ns = spdk_nvme_ctrlr_get_ns(nvme_ctrlr->ctrlr, nsid);
	if (!ns) {
		NVME_NS_DEBUGLOG(nvme_ns, "Invalid NS\n");
		nvme_ns_free(nvme_ns);
		return NULL;
	}

	nvme_ns->id = nsid;
	nvme_ns->ctrlr = nvme_ctrlr;
	nvme_ns->probe_ctx = ctx;
	nvme_ns->ns = ns;
	nvme_ns->ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;

	nvme_ctrlr_get_ref(nvme_ctrlr);
	return nvme_ns;
}

static void
nvme_ns_free(struct nvme_ns *nvme_ns)
{
	free(nvme_ns);
}

static void
nvme_ns_delete(struct nvme_ns *nvme_ns)
{
	nvme_ctrlr_put_ref(nvme_ns->ctrlr);
	nvme_ns_free(nvme_ns);
}

static void nvme_ctrlr_depopulate_namespace(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *nvme_ns);

static void
nvme_ctrlr_populate_namespaces_try_finish(struct nvme_ctrlr *nvme_ctrlr,
		struct nvme_async_probe_ctx **_ctx)
{
	struct nvme_async_probe_ctx *ctx;

	ctx = *_ctx;
	if (!ctx) {
		return;
	}

	*_ctx = NULL;

	assert(ctx->populates_in_progress > 0);
	ctx->populates_in_progress--;
	if (ctx->populates_in_progress == 0) {
		nvme_ctrlr_populate_namespaces_done(nvme_ctrlr, ctx);
	}
}

static void
nvme_ctrlr_populate_namespace_done(struct nvme_ns *nvme_ns, int rc)
{
	assert(spdk_thread_is_app_thread(NULL));

	if (rc) {
		/* Depopulate may be async (ns still on ctrlr list), so defer _try_finish until done. */
		nvme_ctrlr_depopulate_namespace(nvme_ns->ctrlr, nvme_ns);
		return;
	}

	nvme_ctrlr_populate_namespaces_try_finish(nvme_ns->ctrlr, &nvme_ns->probe_ctx);
}

static void
bdev_nvme_add_io_path(struct nvme_bdev_channel_iter *i,
		      struct nvme_bdev *nbdev,
		      struct nvme_bdev_channel *nbdev_ch, void *ctx)
{
	struct nvme_ns *nvme_ns = ctx;
	int rc;

	rc = _bdev_nvme_add_io_path(nbdev_ch, nvme_ns);
	if (rc != 0) {
		NVME_NS_ERRLOG(nvme_ns, "Failed to add I/O path to bdev_channel dynamically.\n");
	}

	nvme_bdev_for_each_channel_continue(i, rc);
}

static void
bdev_nvme_delete_io_path(struct nvme_bdev_channel_iter *i,
			 struct nvme_bdev *nbdev,
			 struct nvme_bdev_channel *nbdev_ch, void *ctx)
{
	struct nvme_ns *nvme_ns = ctx;
	struct nvme_io_path *io_path;

	io_path = _bdev_nvme_get_io_path(nbdev_ch, nvme_ns);
	if (io_path != NULL) {
		_bdev_nvme_delete_io_path(nbdev_ch, io_path);
	}

	nvme_bdev_for_each_channel_continue(i, 0);
}

static void
bdev_nvme_add_io_path_failed(struct nvme_bdev *nbdev, void *ctx, int status)
{
	struct nvme_ns *nvme_ns = ctx;

	nvme_ctrlr_populate_namespace_done(nvme_ns, -1);
}

static void
bdev_nvme_add_io_path_done(struct nvme_bdev *nbdev, void *ctx, int status)
{
	struct nvme_ns *nvme_ns = ctx;

	if (status == 0) {
		nvme_ctrlr_populate_namespace_done(nvme_ns, 0);
	} else {
		/* Delete the added io_paths and fail populating the namespace. */
		nvme_bdev_for_each_channel(nbdev,
					   bdev_nvme_delete_io_path,
					   nvme_ns,
					   bdev_nvme_add_io_path_failed);
	}
}

static int
nvme_bdev_add_ns(struct nvme_bdev *nbdev, struct nvme_ns *nvme_ns)
{
	struct nvme_ns *tmp_ns;
	const struct spdk_nvme_ns_data *nsdata;

	assert(spdk_thread_is_app_thread(NULL));

	nsdata = spdk_nvme_ns_get_data(nvme_ns->ns);
	if (!nsdata->nmic.shrns) {
		NVME_NS_ERRLOG(nvme_ns, "Namespace cannot be shared.\n");
		return -EINVAL;
	}

	tmp_ns = TAILQ_FIRST(&nbdev->nvme_ns_list);
	assert(tmp_ns != NULL);

	if (tmp_ns->ns != NULL && !bdev_nvme_compare_ns(nvme_ns->ns, tmp_ns->ns)) {
		NVME_NS_ERRLOG(nvme_ns, "Namespaces are not identical.\n");
		return -EINVAL;
	}

	nbdev->ref++;

	pthread_mutex_lock(&nbdev->mutex);
	TAILQ_INSERT_TAIL(&nbdev->nvme_ns_list, nvme_ns, tailq);
	pthread_mutex_unlock(&nbdev->mutex);

	nvme_ns->bdev = nbdev;

	/* Add nvme_io_path to nvme_bdev_channels dynamically. */
	nvme_bdev_for_each_channel(nbdev,
				   bdev_nvme_add_io_path,
				   nvme_ns,
				   bdev_nvme_add_io_path_done);

	return 0;
}

static void
nvme_ctrlr_populate_namespace(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *nvme_ns)
{
	struct nvme_bdev	*bdev;
	int			rc = 0;

	if (nvme_ctrlr->ana_log_page != NULL) {
		bdev_nvme_parse_ana_log_page(nvme_ctrlr, nvme_ns_set_ana_state, nvme_ns);
	}

	bdev = nvme_bdev_ctrlr_get_bdev(nvme_ctrlr->nbdev_ctrlr, nvme_ns->id);
	if (bdev == NULL) {
		rc = nvme_bdev_create(nvme_ctrlr, nvme_ns);
	} else {
		rc = nvme_bdev_add_ns(bdev, nvme_ns);
		if (rc == 0) {
			return;
		}
	}

	nvme_ctrlr_populate_namespace_done(nvme_ns, rc);
}

static void
nvme_ctrlr_depopulate_namespace_done(struct nvme_ns *nvme_ns)
{
	struct nvme_ctrlr *nvme_ctrlr = nvme_ns->ctrlr;

	assert(nvme_ctrlr != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	RB_REMOVE(nvme_ns_tree, &nvme_ctrlr->namespaces, nvme_ns);

	if (nvme_ns->bdev != NULL) {
		nvme_ctrlr_populate_namespaces_try_finish(nvme_ctrlr, &nvme_ns->probe_ctx);
		return;
	}

	nvme_ctrlr_populate_namespaces_try_finish(nvme_ctrlr, &nvme_ns->probe_ctx);
	nvme_ns_delete(nvme_ns);
}

static void
bdev_nvme_delete_io_path_done(struct nvme_bdev *nbdev, void *ctx, int status)
{
	struct nvme_ns *nvme_ns = ctx;

	nvme_ctrlr_depopulate_namespace_done(nvme_ns);
}

static void
nvme_ctrlr_depopulate_namespace(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *nvme_ns)
{
	struct nvme_bdev *nbdev;

	assert(spdk_thread_is_app_thread(NULL));

	if (nvme_ns->depopulating) {
		/* Maybe we received 2 AENs in a row */
		return;
	}
	nvme_ns->depopulating = true;

	spdk_poller_unregister(&nvme_ns->anatt_timer);

	nbdev = nvme_ns->bdev;
	if (nbdev != NULL) {
		assert(nbdev->ref > 0);
		nbdev->ref--;
		if (nbdev->ref == 0) {
			spdk_bdev_unregister(&nbdev->disk, NULL, NULL);
		} else {
			/* spdk_bdev_unregister() is not called until the last nvme_ns is
			 * depopulated. Hence we need to remove nvme_ns from bdev->nvme_ns_list
			 * and clear nvme_ns->bdev here.
			 */
			pthread_mutex_lock(&nbdev->mutex);
			TAILQ_REMOVE(&nbdev->nvme_ns_list, nvme_ns, tailq);
			pthread_mutex_unlock(&nbdev->mutex);

			nvme_ns->bdev = NULL;

			/* Delete nvme_io_paths from nvme_bdev_channels dynamically. After that,
			 * we call depopulate_namespace_done() to avoid use-after-free.
			 */
			nvme_bdev_for_each_channel(nbdev,
						   bdev_nvme_delete_io_path,
						   nvme_ns,
						   bdev_nvme_delete_io_path_done);
			return;
		}
	}

	nvme_ctrlr_depopulate_namespace_done(nvme_ns);
}

static void
nvme_ctrlr_populate_namespaces(struct nvme_ctrlr *nvme_ctrlr,
			       struct nvme_async_probe_ctx *ctx)
{
	struct spdk_nvme_ctrlr	*ctrlr = nvme_ctrlr->ctrlr;
	struct nvme_ns	*nvme_ns, *tmp;
	struct spdk_nvme_ns	*ns;
	struct nvme_bdev	*nbdev;
	uint32_t		nsid;
	int			rc;
	uint64_t		num_sectors;

	assert(spdk_thread_is_app_thread(NULL));

	if (ctx) {
		/* Initialize this count to 1 to handle the populate functions
		 * calling nvme_ctrlr_populate_namespace_done() immediately.
		 */
		ctx->populates_in_progress = 1;
	}

	/* First loop over our existing namespaces and see if they have been
	 * changed or removed. */
	RB_FOREACH_SAFE(nvme_ns, nvme_ns_tree, &nvme_ctrlr->namespaces, tmp) {
		if (spdk_nvme_ctrlr_is_active_ns(ctrlr, nvme_ns->id)) {
			/* NS is still there or added again. Its attributes may have changed. */
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nvme_ns->id);
			if (nvme_ns->ns != ns) {
				assert(nvme_ns->ns == NULL);
				nvme_ns->ns = ns;
				NVME_NS_DEBUGLOG(nvme_ns, "NSID was added\n");
			}

			num_sectors = spdk_nvme_ns_get_num_sectors(ns);
			nbdev = nvme_ns->bdev;
			assert(nbdev != NULL);
			if (nbdev->disk.blockcnt != num_sectors) {
				NVME_NS_NOTICELOG(nvme_ns, "NSID is resized: old size %" PRIu64 ", new size %" PRIu64 "\n",
						  nbdev->disk.blockcnt, num_sectors);
				rc = spdk_bdev_notify_blockcnt_change(&nbdev->disk, num_sectors);
				if (rc != 0) {
					NVME_NS_ERRLOG(nvme_ns, "Could not change num blocks for nvme bdev, errno: %d.\n", rc);
				}
			}
		} else {
			/* Namespace was removed */
			nvme_ctrlr_depopulate_namespace(nvme_ctrlr, nvme_ns);
		}
	}

	/* Loop through all of the namespaces at the nvme level and see if any of them are new */
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		nvme_ns = nvme_ctrlr_get_ns(nvme_ctrlr, nsid);
		if (nvme_ns != NULL) {
			continue;
		}

		/* Found a new one */

		ns = spdk_nvme_ctrlr_get_ns(nvme_ctrlr->ctrlr, nsid);
		if (ns == NULL || !spdk_nvme_ns_is_active(ns)) {
			/* Namespace was present during identify controller,
			 * but identify ns was not yet sent. */
			continue;
		}

		nvme_ns = nvme_ns_create(nvme_ctrlr, nsid, ctx);
		if (nvme_ns == NULL) {
			NVME_CTRLR_ERRLOG(nvme_ctrlr, "Failed to allocate namespace\n");
			/* This just fails to attach the namespace. It may work on a future attempt. */
			continue;
		}

		if (ctx) {
			ctx->populates_in_progress++;
		}

		RB_INSERT(nvme_ns_tree, &nvme_ctrlr->namespaces, nvme_ns);
		nvme_ctrlr_populate_namespace(nvme_ctrlr, nvme_ns);
	}

	/* Populate might complete immediately. */
	nvme_ctrlr_populate_namespaces_try_finish(nvme_ctrlr, &ctx);
}

static void
nvme_ctrlr_depopulate_namespaces(struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_ns *nvme_ns, *tmp;

	assert(spdk_thread_is_app_thread(NULL));

	RB_FOREACH_SAFE(nvme_ns, nvme_ns_tree, &nvme_ctrlr->namespaces, tmp) {
		nvme_ctrlr_depopulate_namespace(nvme_ctrlr, nvme_ns);
	}
}

static uint32_t
nvme_ctrlr_get_ana_log_page_size(struct nvme_ctrlr *nvme_ctrlr)
{
	struct spdk_nvme_ctrlr *ctrlr = nvme_ctrlr->ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint32_t nsid, ns_count = 0;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns_count++;
	}

	return sizeof(struct spdk_nvme_ana_page) + cdata->nanagrpid *
	       sizeof(struct spdk_nvme_ana_group_descriptor) + ns_count *
	       sizeof(uint32_t);
}

static int
nvme_ctrlr_set_ana_states(const struct spdk_nvme_ana_group_descriptor *desc,
			  void *cb_arg)
{
	struct nvme_ctrlr *nvme_ctrlr = cb_arg;
	struct nvme_ns *nvme_ns;
	uint32_t i, nsid;

	assert(spdk_thread_is_app_thread(NULL));

	for (i = 0; i < desc->num_of_nsid; i++) {
		nsid = desc->nsid[i];
		if (nsid == 0) {
			continue;
		}

		nvme_ns = nvme_ctrlr_get_ns(nvme_ctrlr, nsid);

		if (nvme_ns == NULL) {
			/* Target told us that an inactive namespace had an ANA change */
			continue;
		}

		_nvme_ns_set_ana_state(nvme_ns, desc);
	}

	return 0;
}

static void
bdev_nvme_disable_read_ana_log_page(struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_ns *nvme_ns;

	assert(spdk_thread_is_app_thread(NULL));

	spdk_free(nvme_ctrlr->ana_log_page);
	nvme_ctrlr->ana_log_page = NULL;

	RB_FOREACH(nvme_ns, nvme_ns_tree, &nvme_ctrlr->namespaces) {
		nvme_ns->ana_state_updating = false;
		nvme_ns->ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	}
}

static void
nvme_ctrlr_read_ana_log_page_done(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	if (cpl != NULL && spdk_nvme_cpl_is_success(cpl)) {
		bdev_nvme_parse_ana_log_page(nvme_ctrlr, nvme_ctrlr_set_ana_states,
					     nvme_ctrlr);
	} else {
		bdev_nvme_disable_read_ana_log_page(nvme_ctrlr);
	}

	pthread_mutex_lock(&nvme_ctrlr->mutex);
	assert(nvme_ctrlr->ana_log_page_updating == true);
	nvme_ctrlr->ana_log_page_updating = false;
	nvme_ctrlr_put_ref_ext(nvme_ctrlr, bdev_nvme_clear_io_path_caches);
	pthread_mutex_unlock(&nvme_ctrlr->mutex);
}

static int
nvme_ctrlr_read_ana_log_page(struct nvme_ctrlr *nvme_ctrlr)
{
	uint32_t ana_log_page_size;
	int rc;

	if (nvme_ctrlr->ana_log_page == NULL) {
		return -EINVAL;
	}

	ana_log_page_size = nvme_ctrlr_get_ana_log_page_size(nvme_ctrlr);

	if (ana_log_page_size > nvme_ctrlr->max_ana_log_page_size) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr,
				  "ANA log page size %" PRIu32 " is larger than allowed %" PRIu32 "\n",
				  ana_log_page_size, nvme_ctrlr->max_ana_log_page_size);
		return -EINVAL;
	}

	pthread_mutex_lock(&nvme_ctrlr->mutex);
	if (!nvme_ctrlr_is_available(nvme_ctrlr) ||
	    nvme_ctrlr->ana_log_page_updating) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return -EBUSY;
	}

	nvme_ctrlr->ana_log_page_updating = true;
	nvme_ctrlr_get_ref(nvme_ctrlr);
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	rc = spdk_nvme_ctrlr_cmd_get_log_page(nvme_ctrlr->ctrlr,
					      SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS,
					      SPDK_NVME_GLOBAL_NS_TAG,
					      nvme_ctrlr->ana_log_page,
					      ana_log_page_size, 0,
					      nvme_ctrlr_read_ana_log_page_done,
					      nvme_ctrlr);
	if (rc != 0) {
		nvme_ctrlr_read_ana_log_page_done(nvme_ctrlr, NULL);
	}

	return rc;
}

static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

struct bdev_nvme_set_preferred_path_ctx {
	struct spdk_bdev_desc *desc;
	struct nvme_ns *nvme_ns;
	bdev_nvme_set_preferred_path_cb cb_fn;
	void *cb_arg;
};

static void
bdev_nvme_set_preferred_path_done(struct nvme_bdev *nbdev, void *_ctx, int status)
{
	struct bdev_nvme_set_preferred_path_ctx *ctx = _ctx;

	assert(ctx != NULL);
	assert(ctx->desc != NULL);
	assert(ctx->cb_fn != NULL);

	spdk_bdev_close(ctx->desc);

	ctx->cb_fn(ctx->cb_arg, status);

	free(ctx);
}

static void
_bdev_nvme_set_preferred_path(struct nvme_bdev_channel_iter *i,
			      struct nvme_bdev *nbdev,
			      struct nvme_bdev_channel *nbdev_ch, void *_ctx)
{
	struct bdev_nvme_set_preferred_path_ctx *ctx = _ctx;
	struct nvme_io_path *io_path, *prev;

	prev = NULL;
	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		if (io_path->nvme_ns == ctx->nvme_ns) {
			break;
		}
		prev = io_path;
	}

	if (io_path != NULL) {
		if (prev != NULL) {
			STAILQ_REMOVE_AFTER(&nbdev_ch->io_path_list, prev, stailq);
			STAILQ_INSERT_HEAD(&nbdev_ch->io_path_list, io_path, stailq);
		}

		/* We can set io_path to nbdev_ch->current_io_path directly here.
		 * However, it needs to be conditional. To simplify the code,
		 * just clear nbdev_ch->current_io_path and let find_io_path()
		 * fill it.
		 *
		 * Automatic failback may be disabled. Hence even if the io_path is
		 * already at the head, clear nbdev_ch->current_io_path.
		 */
		bdev_nvme_clear_current_io_path(nbdev_ch);
	}

	nvme_bdev_for_each_channel_continue(i, 0);
}

static struct nvme_ns *
bdev_nvme_set_preferred_ns(struct nvme_bdev *nbdev, uint16_t cntlid)
{
	struct nvme_ns *nvme_ns, *prev;
	const struct spdk_nvme_ctrlr_data *cdata;

	assert(spdk_thread_is_app_thread(NULL));

	prev = NULL;
	TAILQ_FOREACH(nvme_ns, &nbdev->nvme_ns_list, tailq) {
		cdata = spdk_nvme_ctrlr_get_data(nvme_ns->ctrlr->ctrlr);

		if (cdata->cntlid == cntlid) {
			break;
		}
		prev = nvme_ns;
	}

	if (nvme_ns != NULL && prev != NULL) {
		pthread_mutex_lock(&nbdev->mutex);
		TAILQ_REMOVE(&nbdev->nvme_ns_list, nvme_ns, tailq);
		TAILQ_INSERT_HEAD(&nbdev->nvme_ns_list, nvme_ns, tailq);
		pthread_mutex_unlock(&nbdev->mutex);
	}

	return nvme_ns;
}

/* This function supports only multipath mode. There is only a single I/O path
 * for each NVMe-oF controller. Hence, just move the matched I/O path to the
 * head of the I/O path list for each NVMe bdev channel.
 *
 * NVMe bdev channel may be acquired after completing this function. move the
 * matched namespace to the head of the namespace list for the NVMe bdev too.
 */
/*
 * [한국어]
 * bdev_nvme_set_preferred_path - bdev_nvme.h §2 참조. cntlid가 가리키는 컨트롤러를 우선 경로로 설정.
 *
 * 동작:
 *   1) bdev open → 그 nvme_bdev에서 cntlid 매치 namespace를 nvme_ns_list 맨 앞으로 이동.
 *   2) 모든 채널에 메시지를 보내 io_path_list에서도 같은 path를 맨 앞으로 + current_io_path 클리어.
 *   3) 다음 IO부터 새 preferred path가 사용됨.
 * Failover 모드는 미지원 (영문 주석 참조). 컨텍스트: app 스레드 강제.
 */
void
bdev_nvme_set_preferred_path(const char *name, uint16_t cntlid,
			     bdev_nvme_set_preferred_path_cb cb_fn, void *cb_arg)
{
	struct bdev_nvme_set_preferred_path_ctx *ctx;
	struct spdk_bdev *bdev;
	struct nvme_bdev *nbdev;
	int rc = 0;

	assert(cb_fn != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to alloc context.\n");
		rc = -ENOMEM;
		goto err_alloc;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = spdk_bdev_open_ext(name, false, dummy_bdev_event_cb, NULL, &ctx->desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev %s.\n", name);
		goto err_open;
	}

	bdev = spdk_bdev_desc_get_bdev(ctx->desc);

	if (bdev->module != &nvme_if) {
		SPDK_ERRLOG("bdev %s is not registered in this module.\n", name);
		rc = -ENODEV;
		goto err_bdev;
	}

	nbdev = SPDK_CONTAINEROF(bdev, struct nvme_bdev, disk);

	ctx->nvme_ns = bdev_nvme_set_preferred_ns(nbdev, cntlid);
	if (ctx->nvme_ns == NULL) {
		NVME_BDEV_ERRLOG(nbdev, null_ctrlr, "bdev does not have namespace to controller %u.\n", cntlid);
		rc = -ENODEV;
		goto err_bdev;
	}

	nvme_bdev_for_each_channel(nbdev,
				   _bdev_nvme_set_preferred_path,
				   ctx,
				   bdev_nvme_set_preferred_path_done);
	return;

err_bdev:
	spdk_bdev_close(ctx->desc);
err_open:
	free(ctx);
err_alloc:
	cb_fn(cb_arg, rc);
}

struct bdev_nvme_set_multipath_policy_ctx {
	struct spdk_bdev_desc *desc;
	spdk_bdev_nvme_set_multipath_policy_cb cb_fn;
	void *cb_arg;
};

static void
bdev_nvme_set_multipath_policy_done(struct nvme_bdev *nbdev, void *_ctx, int status)
{
	struct bdev_nvme_set_multipath_policy_ctx *ctx = _ctx;

	assert(ctx != NULL);
	assert(ctx->desc != NULL);
	assert(ctx->cb_fn != NULL);

	spdk_bdev_close(ctx->desc);

	ctx->cb_fn(ctx->cb_arg, status);

	free(ctx);
}

static void
_bdev_nvme_set_multipath_policy(struct nvme_bdev_channel_iter *i,
				struct nvme_bdev *nbdev,
				struct nvme_bdev_channel *nbdev_ch, void *ctx)
{
	nbdev_ch->mp_policy = nbdev->mp_policy;
	nbdev_ch->mp_selector = nbdev->mp_selector;
	nbdev_ch->rr_min_io = nbdev->rr_min_io;
	bdev_nvme_clear_current_io_path(nbdev_ch);

	nvme_bdev_for_each_channel_continue(i, 0);
}

void
spdk_bdev_nvme_set_multipath_policy(const char *name, enum spdk_bdev_nvme_multipath_policy policy,
				    enum spdk_bdev_nvme_multipath_selector selector, uint32_t rr_min_io,
				    spdk_bdev_nvme_set_multipath_policy_cb cb_fn, void *cb_arg)
{
	struct bdev_nvme_set_multipath_policy_ctx *ctx;
	struct spdk_bdev *bdev;
	struct nvme_bdev *nbdev;
	int rc;

	assert(cb_fn != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	switch (policy) {
	case BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE:
		break;
	case BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE:
		switch (selector) {
		case BDEV_NVME_MP_SELECTOR_ROUND_ROBIN:
			if (rr_min_io == UINT32_MAX) {
				rr_min_io = 1;
			} else if (rr_min_io == 0) {
				rc = -EINVAL;
				goto exit;
			}
			break;
		case BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH:
			break;
		default:
			rc = -EINVAL;
			goto exit;
		}
		break;
	default:
		rc = -EINVAL;
		goto exit;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to alloc context.\n");
		rc = -ENOMEM;
		goto exit;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = spdk_bdev_open_ext(name, false, dummy_bdev_event_cb, NULL, &ctx->desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev %s.\n", name);
		rc = -ENODEV;
		goto err_open;
	}

	bdev = spdk_bdev_desc_get_bdev(ctx->desc);
	if (bdev->module != &nvme_if) {
		SPDK_ERRLOG("bdev %s is not registered in this module.\n", name);
		rc = -ENODEV;
		goto err_module;
	}
	nbdev = SPDK_CONTAINEROF(bdev, struct nvme_bdev, disk);

	pthread_mutex_lock(&nbdev->mutex);
	nbdev->mp_policy = policy;
	nbdev->mp_selector = selector;
	nbdev->rr_min_io = rr_min_io;
	pthread_mutex_unlock(&nbdev->mutex);

	nvme_bdev_for_each_channel(nbdev,
				   _bdev_nvme_set_multipath_policy,
				   ctx,
				   bdev_nvme_set_multipath_policy_done);
	return;

err_module:
	spdk_bdev_close(ctx->desc);
err_open:
	free(ctx);
exit:
	cb_fn(cb_arg, rc);
}

static void
nvme_ctrlr_aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr *nvme_ctrlr		= arg;
	union spdk_nvme_async_event_completion	event;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVME_CTRLR_WARNLOG(nvme_ctrlr, "AER request execute failed\n");
		return;
	}

	NVME_CTRLR_DEBUGLOG(nvme_ctrlr, "executing AER\n");
	event.raw = cpl->cdw0;
	if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
	    (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED)) {
		nvme_ctrlr_populate_namespaces(nvme_ctrlr, NULL);
	} else if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
		   (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_ANA_CHANGE)) {
		nvme_ctrlr_read_ana_log_page(nvme_ctrlr);
	}
}

static void
free_nvme_async_probe_ctx(struct nvme_async_probe_ctx *ctx)
{
	spdk_keyring_put_key(ctx->drv_opts.tls_psk);
	spdk_keyring_put_key(ctx->drv_opts.dhchap_key);
	spdk_keyring_put_key(ctx->drv_opts.dhchap_ctrlr_key);
	free(ctx->base_name);
	free(ctx);
}

static void
populate_namespaces_cb(struct nvme_async_probe_ctx *ctx, int rc)
{
	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_ctx, ctx->reported_bdevs, rc);
	}

	ctx->namespaces_populated = true;
	if (ctx->probe_done) {
		/* The probe was already completed, so we need to free the context
		 * here.  This can happen for cases like OCSSD, where we need to
		 * send additional commands to the SSD after attach.
		 */
		free_nvme_async_probe_ctx(ctx);
	}
}

static int
bdev_nvme_remove_poller(void *ctx)
{
	struct spdk_nvme_transport_id trid_pcie;

	if (TAILQ_EMPTY(&g_nvme_bdev_ctrlrs)) {
		spdk_poller_unregister(&g_hotplug_poller);
		return SPDK_POLLER_IDLE;
	}

	memset(&trid_pcie, 0, sizeof(trid_pcie));
	spdk_nvme_trid_populate_transport(&trid_pcie, SPDK_NVME_TRANSPORT_PCIE);

	if (spdk_nvme_scan_attached(&trid_pcie)) {
		SPDK_ERRLOG_RATELIMIT("spdk_nvme_scan_attached() failed\n");
	}

	return SPDK_POLLER_BUSY;
}

static void
nvme_ctrlr_create_done(struct nvme_ctrlr *nvme_ctrlr,
		       struct nvme_async_probe_ctx *ctx)
{
	NVME_CTRLR_INFOLOG(nvme_ctrlr, "ctrlr was created\n");

	/* AER callback is registered late to prevent getting the I/O channel
	 * on an unregistered controller during namespace population. */
	spdk_nvme_ctrlr_register_aer_callback(nvme_ctrlr->ctrlr, nvme_ctrlr_aer_cb, nvme_ctrlr);

	/* Populate namespaces for the first time. */
	nvme_ctrlr_populate_namespaces(nvme_ctrlr, ctx);

	if (g_hotplug_poller == NULL) {
		g_hotplug_poller = SPDK_POLLER_REGISTER(bdev_nvme_remove_poller, NULL,
							NVME_HOTPLUG_POLL_PERIOD_DEFAULT);
	}
}

static void
nvme_ctrlr_init_ana_log_page_done(void *_ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr *nvme_ctrlr = _ctx;
	struct nvme_async_probe_ctx *ctx = nvme_ctrlr->probe_ctx;

	nvme_ctrlr->probe_ctx = NULL;

	if (spdk_nvme_cpl_is_error(cpl)) {
		bdev_nvme_delete_ctrlr(nvme_ctrlr, false);

		if (ctx != NULL) {
			ctx->reported_bdevs = 0;
			populate_namespaces_cb(ctx, -1);
		}
		return;
	}

	nvme_ctrlr_create_done(nvme_ctrlr, ctx);
}

static int
nvme_ctrlr_init_ana_log_page(struct nvme_ctrlr *nvme_ctrlr,
			     struct nvme_async_probe_ctx *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = nvme_ctrlr->ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint32_t ana_log_page_size;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	/* Set buffer size enough to include maximum number of allowed namespaces. */
	ana_log_page_size = sizeof(struct spdk_nvme_ana_page) + cdata->nanagrpid *
			    sizeof(struct spdk_nvme_ana_group_descriptor) + cdata->mnan *
			    sizeof(uint32_t);

	nvme_ctrlr->ana_log_page = spdk_zmalloc(ana_log_page_size, 64, NULL,
						SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (nvme_ctrlr->ana_log_page == NULL) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "could not allocate ANA log page buffer\n");
		return -ENXIO;
	}

	/* Each descriptor in a ANA log page is not ensured to be 8-bytes aligned.
	 * Hence copy each descriptor to a temporary area when parsing it.
	 *
	 * Allocate a buffer whose size is as large as ANA log page buffer because
	 * we do not know the size of a descriptor until actually reading it.
	 */
	nvme_ctrlr->copied_ana_desc = calloc(1, ana_log_page_size);
	if (nvme_ctrlr->copied_ana_desc == NULL) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "could not allocate a buffer to parse ANA descriptor\n");
		return -ENOMEM;
	}

	nvme_ctrlr->max_ana_log_page_size = ana_log_page_size;

	nvme_ctrlr->probe_ctx = ctx;

	/* Then, set the read size only to include the current active namespaces. */
	ana_log_page_size = nvme_ctrlr_get_ana_log_page_size(nvme_ctrlr);

	if (ana_log_page_size > nvme_ctrlr->max_ana_log_page_size) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "ANA log page size %" PRIu32 " is larger than allowed %" PRIu32 "\n",
				  ana_log_page_size, nvme_ctrlr->max_ana_log_page_size);
		return -EINVAL;
	}

	return spdk_nvme_ctrlr_cmd_get_log_page(ctrlr,
						SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS,
						SPDK_NVME_GLOBAL_NS_TAG,
						nvme_ctrlr->ana_log_page,
						ana_log_page_size, 0,
						nvme_ctrlr_init_ana_log_page_done,
						nvme_ctrlr);
}

/* hostnqn and subnqn were already verified before attaching a controller.
 * Hence check only the multipath capability and cntlid here.
 */
static bool
bdev_nvme_check_multipath(struct nvme_bdev_ctrlr *nbdev_ctrlr, struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_ctrlr *tmp;
	const struct spdk_nvme_ctrlr_data *cdata, *tmp_cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!cdata->cmic.mctrs) {
		SPDK_ERRLOG("Ctrlr%u does not support multipath.\n", cdata->cntlid);
		return false;
	}

	TAILQ_FOREACH(tmp, &nbdev_ctrlr->ctrlrs, tailq) {
		tmp_cdata = spdk_nvme_ctrlr_get_data(tmp->ctrlr);

		if (!tmp_cdata->cmic.mctrs) {
			NVME_CTRLR_ERRLOG(tmp, "Ctrlr%u does not support multipath.\n", cdata->cntlid);
			return false;
		}
		if (cdata->cntlid == tmp_cdata->cntlid) {
			NVME_CTRLR_ERRLOG(tmp, "cntlid %u are duplicated.\n", tmp_cdata->cntlid);
			return false;
		}
	}

	return true;
}


static int
nvme_bdev_ctrlr_create(const char *name, struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct spdk_nvme_ctrlr *ctrlr = nvme_ctrlr->ctrlr;
	struct nvme_ctrlr      *nctrlr;

	assert(spdk_thread_is_app_thread(NULL));

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (nbdev_ctrlr != NULL) {
		if (!bdev_nvme_check_multipath(nbdev_ctrlr, ctrlr)) {
			return -EINVAL;
		}
		TAILQ_FOREACH(nctrlr, &nbdev_ctrlr->ctrlrs, tailq) {
			if (nctrlr->opts.multipath != nvme_ctrlr->opts.multipath) {
				/* All controllers with the same name must be configured the same
				 * way, either for multipath or failover. If the configuration doesn't
				 * match - report error.
				 */
				return -EINVAL;
			}
		}
	} else {
		nbdev_ctrlr = calloc(1, sizeof(*nbdev_ctrlr));
		if (nbdev_ctrlr == NULL) {
			NVME_CTRLR_ERRLOG(nvme_ctrlr, "Failed to allocate nvme_bdev_ctrlr.\n");
			return -ENOMEM;
		}
		nbdev_ctrlr->name = strdup(name);
		if (nbdev_ctrlr->name == NULL) {
			NVME_CTRLR_ERRLOG(nvme_ctrlr, "Failed to allocate name of nvme_bdev_ctrlr.\n");
			free(nbdev_ctrlr);
			return -ENOMEM;
		}
		TAILQ_INIT(&nbdev_ctrlr->ctrlrs);
		TAILQ_INIT(&nbdev_ctrlr->bdevs);
		TAILQ_INSERT_TAIL(&g_nvme_bdev_ctrlrs, nbdev_ctrlr, tailq);
	}
	nvme_ctrlr->nbdev_ctrlr = nbdev_ctrlr;
	TAILQ_INSERT_TAIL(&nbdev_ctrlr->ctrlrs, nvme_ctrlr, tailq);
	return 0;
}

static int
nvme_ctrlr_mutex_init(pthread_mutex_t *mtx)
{
	pthread_mutexattr_t attr;
	int rc = 0;

	if (pthread_mutexattr_init(&attr)) {
		return -1;
	}

	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) || pthread_mutex_init(mtx, &attr)) {
		rc = -1;
	}

	pthread_mutexattr_destroy(&attr);
	return rc;
}

static int
nvme_ctrlr_create(struct spdk_nvme_ctrlr *ctrlr,
		  const char *name,
		  const struct spdk_nvme_transport_id *trid,
		  struct nvme_async_probe_ctx *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr;
	struct spdk_nvme_path_id *path_id;
	const struct spdk_nvme_ctrlr_data *cdata;
	struct spdk_event_handler_opts opts = {
		.opts_size = SPDK_SIZEOF(&opts, fd_type),
	};
	uint64_t period;
	int fd, rc;

	assert(spdk_thread_is_app_thread(NULL));

	nvme_ctrlr = calloc(1, sizeof(*nvme_ctrlr));
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return -ENOMEM;
	}

	rc = nvme_ctrlr_mutex_init(&nvme_ctrlr->mutex);
	if (rc != 0) {
		free(nvme_ctrlr);
		return rc;
	}

	TAILQ_INIT(&nvme_ctrlr->trids);
	TAILQ_INIT(&nvme_ctrlr->pending_resets);
	RB_INIT(&nvme_ctrlr->namespaces);

	/* Get another reference to the key, so the first one can be released from probe_ctx */
	if (ctx != NULL) {
		if (ctx->drv_opts.tls_psk != NULL) {
			nvme_ctrlr->psk = spdk_keyring_get_key(
						  spdk_key_get_name(ctx->drv_opts.tls_psk));
			if (nvme_ctrlr->psk == NULL) {
				/* Could only happen if the key was removed in the meantime */
				SPDK_ERRLOG("Couldn't get a reference to the key '%s'\n",
					    spdk_key_get_name(ctx->drv_opts.tls_psk));
				rc = -ENOKEY;
				goto err;
			}
		}

		if (ctx->drv_opts.dhchap_key != NULL) {
			nvme_ctrlr->dhchap_key = spdk_keyring_get_key(
							 spdk_key_get_name(ctx->drv_opts.dhchap_key));
			if (nvme_ctrlr->dhchap_key == NULL) {
				SPDK_ERRLOG("Couldn't get a reference to the key '%s'\n",
					    spdk_key_get_name(ctx->drv_opts.dhchap_key));
				rc = -ENOKEY;
				goto err;
			}
		}

		if (ctx->drv_opts.dhchap_ctrlr_key != NULL) {
			nvme_ctrlr->dhchap_ctrlr_key =
				spdk_keyring_get_key(
					spdk_key_get_name(ctx->drv_opts.dhchap_ctrlr_key));
			if (nvme_ctrlr->dhchap_ctrlr_key == NULL) {
				SPDK_ERRLOG("Couldn't get a reference to the key '%s'\n",
					    spdk_key_get_name(ctx->drv_opts.dhchap_ctrlr_key));
				rc = -ENOKEY;
				goto err;
			}
		}
	}

	/* Check if we manage to enable interrupts on the controller. */
	if (spdk_interrupt_mode_is_enabled() && ctx != NULL && !ctx->drv_opts.enable_interrupts) {
		SPDK_ERRLOG("Failed to enable interrupts on the controller\n");
		rc = -ENOTSUP;
		goto err;
	}

	path_id = calloc(1, sizeof(*path_id));
	if (path_id == NULL) {
		SPDK_ERRLOG("Failed to allocate trid entry pointer\n");
		rc = -ENOMEM;
		goto err;
	}

	path_id->trid = *trid;
	if (ctx != NULL) {
		memcpy(path_id->hostid.hostaddr, ctx->drv_opts.src_addr, sizeof(path_id->hostid.hostaddr));
		memcpy(path_id->hostid.hostsvcid, ctx->drv_opts.src_svcid, sizeof(path_id->hostid.hostsvcid));
	}
	nvme_ctrlr->active_path_id = path_id;
	TAILQ_INSERT_HEAD(&nvme_ctrlr->trids, path_id, link);

	nvme_ctrlr->ctrlr = ctrlr;
	nvme_ctrlr->ref = 1;

	if (spdk_nvme_ctrlr_is_ocssd_supported(ctrlr)) {
		SPDK_ERRLOG("OCSSDs are not supported");
		rc = -ENOTSUP;
		goto err;
	}

	if (ctx != NULL) {
		memcpy(&nvme_ctrlr->opts, &ctx->bdev_opts, sizeof(ctx->bdev_opts));
	} else {
		spdk_bdev_nvme_get_default_ctrlr_opts(&nvme_ctrlr->opts);
	}

	period = spdk_interrupt_mode_is_enabled() ? 0 : g_opts.nvme_adminq_poll_period_us;

	nvme_ctrlr->adminq_timer_poller = SPDK_POLLER_REGISTER(bdev_nvme_poll_adminq, nvme_ctrlr,
					  period);

	if (spdk_interrupt_mode_is_enabled()) {
		spdk_poller_register_interrupt(nvme_ctrlr->adminq_timer_poller, NULL, NULL);

		fd = spdk_nvme_ctrlr_get_admin_qp_fd(nvme_ctrlr->ctrlr, &opts);
		if (fd < 0) {
			rc = fd;
			goto err;
		}

		nvme_ctrlr->intr = SPDK_INTERRUPT_REGISTER_EXT(fd, bdev_nvme_poll_adminq,
				   nvme_ctrlr, &opts);
		if (!nvme_ctrlr->intr) {
			rc = -EINVAL;
			goto err;
		}
	}

	if (g_opts.timeout_us > 0) {
		/* Register timeout callback. Timeout values for IO vs. admin reqs can be different. */
		/* If timeout_admin_us is 0 (not specified), admin uses same timeout as IO. */
		uint64_t adm_timeout_us = (g_opts.timeout_admin_us == 0) ?
					  g_opts.timeout_us : g_opts.timeout_admin_us;
		spdk_nvme_ctrlr_register_timeout_callback(ctrlr, g_opts.timeout_us,
				adm_timeout_us, timeout_cb, nvme_ctrlr);
	}

	spdk_nvme_ctrlr_set_remove_cb(ctrlr, remove_cb, nvme_ctrlr);

	if (spdk_nvme_ctrlr_get_flags(ctrlr) &
	    SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		nvme_ctrlr->opal_dev = spdk_opal_dev_construct(ctrlr);
	}

	rc = nvme_bdev_ctrlr_create(name, nvme_ctrlr);
	if (rc != 0) {
		goto err;
	}

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	if (cdata->cmic.anars) {
		rc = nvme_ctrlr_init_ana_log_page(nvme_ctrlr, ctx);
		if (rc != 0) {
			goto err;
		}
	}

	/* Register the I/O device early because, on the negative path handling of the admin qpair, many
	 * flows iterate through nvme_ctrlr channels, and the same applies to some JSON-RPC methods. If
	 * the device is not registered, this triggers assertions and returns a negative status. With
	 * early registration, these flows simply iterate through zero channels and return success. */
	spdk_io_device_register(nvme_ctrlr,
				bdev_nvme_create_ctrlr_channel_cb,
				bdev_nvme_destroy_ctrlr_channel_cb,
				sizeof(struct nvme_ctrlr_channel),
				nvme_ctrlr->nbdev_ctrlr->name);

	if (!cdata->cmic.anars) {
		nvme_ctrlr_create_done(nvme_ctrlr, ctx);
	}

	return 0;

err:
	nvme_ctrlr_delete(nvme_ctrlr);
	return rc;
}

void
spdk_bdev_nvme_get_default_ctrlr_opts(struct spdk_bdev_nvme_ctrlr_opts *opts)
{
	opts->prchk_flags = 0;
	opts->ctrlr_loss_timeout_sec = g_opts.ctrlr_loss_timeout_sec;
	opts->reconnect_delay_sec = g_opts.reconnect_delay_sec;
	opts->fast_io_fail_timeout_sec = g_opts.fast_io_fail_timeout_sec;
	opts->multipath = true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *drv_opts)
{
	char *name;

	name = spdk_sprintf_alloc("HotInNvme%d", g_hot_insert_nvme_controller_index++);
	if (!name) {
		SPDK_ERRLOG("Failed to assign name to NVMe device\n");
		return;
	}

	if (nvme_ctrlr_create(ctrlr, name, trid, NULL) == 0) {
		SPDK_DEBUGLOG(bdev_nvme, "Attached to %s (%s)\n", trid->traddr, name);
	} else {
		SPDK_ERRLOG("Failed to attach to %s (%s)\n", trid->traddr, name);
	}

	free(name);
}

static void
_nvme_ctrlr_destruct(void *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	NVME_CTRLR_INFOLOG(nvme_ctrlr, "destructing ctrlr\n");
	nvme_ctrlr_depopulate_namespaces(nvme_ctrlr);
	nvme_ctrlr_put_ref(nvme_ctrlr);
}

static int
bdev_nvme_delete_ctrlr_unsafe(struct nvme_ctrlr *nvme_ctrlr, bool hotplug)
{
	struct nvme_probe_skip_entry *entry;

	/* The controller's destruction was already started */
	if (nvme_ctrlr->destruct) {
		return -EALREADY;
	}

	if (!hotplug &&
	    nvme_ctrlr->active_path_id->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		entry = calloc(1, sizeof(*entry));
		if (!entry) {
			return -ENOMEM;
		}
		entry->trid = nvme_ctrlr->active_path_id->trid;
		TAILQ_INSERT_TAIL(&g_skipped_nvme_ctrlrs, entry, tailq);
	}

	nvme_ctrlr->destruct = true;
	return 0;
}

static int
bdev_nvme_delete_ctrlr(struct nvme_ctrlr *nvme_ctrlr, bool hotplug)
{
	int rc;

	pthread_mutex_lock(&nvme_ctrlr->mutex);
	rc = bdev_nvme_delete_ctrlr_unsafe(nvme_ctrlr, hotplug);
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	if (rc == 0) {
		_nvme_ctrlr_destruct(nvme_ctrlr);
	} else if (rc == -EALREADY) {
		rc = 0;
	}

	return rc;
}

static void
remove_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_ctrlr *nvme_ctrlr = cb_ctx;

	bdev_nvme_delete_ctrlr(nvme_ctrlr, true);
}

static int
bdev_nvme_hotplug_probe(void *arg)
{
	if (g_hotplug_probe_ctx == NULL) {
		spdk_poller_unregister(&g_hotplug_probe_poller);
		return SPDK_POLLER_IDLE;
	}

	if (spdk_nvme_probe_poll_async(g_hotplug_probe_ctx) != -EAGAIN) {
		g_hotplug_probe_ctx = NULL;
		spdk_poller_unregister(&g_hotplug_probe_poller);
	}

	return SPDK_POLLER_BUSY;
}

static int
bdev_nvme_hotplug(void *arg)
{
	struct spdk_nvme_transport_id trid_pcie;

	if (g_hotplug_probe_ctx) {
		return SPDK_POLLER_BUSY;
	}

	memset(&trid_pcie, 0, sizeof(trid_pcie));
	spdk_nvme_trid_populate_transport(&trid_pcie, SPDK_NVME_TRANSPORT_PCIE);

	g_hotplug_probe_ctx = spdk_nvme_probe_async(&trid_pcie, NULL,
			      hotplug_probe_cb, attach_cb, NULL);

	if (g_hotplug_probe_ctx) {
		assert(g_hotplug_probe_poller == NULL);
		g_hotplug_probe_poller = SPDK_POLLER_REGISTER(bdev_nvme_hotplug_probe, NULL, 1000);
	}

	return SPDK_POLLER_BUSY;
}

void
spdk_bdev_nvme_get_opts(struct spdk_bdev_nvme_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	opts->opts_size = opts_size;

#define SET_FIELD(field, defval) \
		opts->field = SPDK_GET_FIELD(&g_opts, field, defval, opts_size); \

	SET_FIELD(action_on_timeout, 0);
	SET_FIELD(keep_alive_timeout_ms, 0);
	SET_FIELD(timeout_us, 0);
	SET_FIELD(timeout_admin_us, 0);
	SET_FIELD(transport_retry_count, 0);
	SET_FIELD(arbitration_burst, 0);
	SET_FIELD(low_priority_weight, 0);
	SET_FIELD(medium_priority_weight, 0);
	SET_FIELD(high_priority_weight, 0);
	SET_FIELD(io_queue_requests, 0);
	SET_FIELD(nvme_adminq_poll_period_us, 0);
	SET_FIELD(nvme_ioq_poll_period_us, 0);
	SET_FIELD(delay_cmd_submit, 0);
	SET_FIELD(bdev_retry_count, 0);
	SET_FIELD(ctrlr_loss_timeout_sec, 0);
	SET_FIELD(reconnect_delay_sec, 0);
	SET_FIELD(fast_io_fail_timeout_sec, 0);
	SET_FIELD(transport_ack_timeout, 0);
	SET_FIELD(disable_auto_failback, false);
	SET_FIELD(generate_uuids, false);
	SET_FIELD(transport_tos, 0);
	SET_FIELD(nvme_error_stat, false);
	SET_FIELD(io_path_stat, false);
	SET_FIELD(allow_accel_sequence, false);
	SET_FIELD(rdma_srq_size, 0);
	SET_FIELD(rdma_max_cq_size, 0);
	SET_FIELD(rdma_cm_event_timeout_ms, 0);
	SET_FIELD(dhchap_digests, 0);
	SET_FIELD(dhchap_dhgroups, 0);
	SET_FIELD(rdma_umr_per_io, false);
	SET_FIELD(tcp_connect_timeout_ms, 0);
	SET_FIELD(enable_flush, false);

#undef SET_FIELD

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_nvme_opts) == 136, "Incorrect size");
}

static bool bdev_nvme_check_io_error_resiliency_params(int32_t ctrlr_loss_timeout_sec,
		uint32_t reconnect_delay_sec,
		uint32_t fast_io_fail_timeout_sec);

static int
bdev_nvme_validate_opts(const struct spdk_bdev_nvme_opts *opts)
{
	if ((opts->timeout_us == 0) && (opts->timeout_admin_us != 0)) {
		/* Can't set timeout_admin_us without also setting timeout_us */
		SPDK_WARNLOG("Invalid options: Can't have (timeout_us == 0) with (timeout_admin_us > 0)\n");
		return -EINVAL;
	}

	if (opts->timeout_us &&
	    opts->keep_alive_timeout_ms * SPDK_MSEC_TO_USEC > opts->timeout_us) {
		SPDK_WARNLOG("keep_alive_timeout_ms %u should be less than timeout_us %lu\n",
			     opts->keep_alive_timeout_ms, opts->timeout_us);
	}

	if (opts->timeout_admin_us &&
	    opts->keep_alive_timeout_ms * SPDK_MSEC_TO_USEC > opts->timeout_admin_us) {
		SPDK_WARNLOG("keep_alive_timeout_ms %u should be less than timeout_admin_us %lu\n",
			     opts->keep_alive_timeout_ms, opts->timeout_admin_us);
	}

	if (opts->bdev_retry_count < -1) {
		SPDK_WARNLOG("Invalid option: bdev_retry_count can't be less than -1.\n");
		return -EINVAL;
	}

	if (!bdev_nvme_check_io_error_resiliency_params(opts->ctrlr_loss_timeout_sec,
			opts->reconnect_delay_sec,
			opts->fast_io_fail_timeout_sec)) {
		return -EINVAL;
	}

	return 0;
}

int
spdk_bdev_nvme_set_opts(const struct spdk_bdev_nvme_opts *opts)
{
	struct spdk_nvme_transport_opts drv_opts;
	int ret;

	if (!opts) {
		SPDK_ERRLOG("opts cannot be NULL\n");
		return -1;
	}

	if (!opts->opts_size) {
		SPDK_ERRLOG("opts_size inside opts cannot be zero value\n");
		return -1;
	}

	ret = bdev_nvme_validate_opts(opts);
	if (ret) {
		SPDK_WARNLOG("Failed to set nvme opts.\n");
		return ret;
	}

	if (g_bdev_nvme_init_done && !TAILQ_EMPTY(&g_nvme_bdev_ctrlrs)) {
		return -EPERM;
	}

	spdk_nvme_transport_get_opts(&drv_opts, sizeof(drv_opts));
	if (opts->rdma_srq_size != 0) {
		drv_opts.rdma_srq_size = opts->rdma_srq_size;
	}
	if (opts->rdma_max_cq_size != 0) {
		drv_opts.rdma_max_cq_size = opts->rdma_max_cq_size;
	}
	if (opts->rdma_cm_event_timeout_ms != 0) {
		drv_opts.rdma_cm_event_timeout_ms = opts->rdma_cm_event_timeout_ms;
	}
	if (drv_opts.rdma_umr_per_io != opts->rdma_umr_per_io) {
		drv_opts.rdma_umr_per_io = opts->rdma_umr_per_io;
	}
	if (opts->tcp_connect_timeout_ms != 0) {
		drv_opts.tcp_connect_timeout_ms = opts->tcp_connect_timeout_ms;
	}
	ret = spdk_nvme_transport_set_opts(&drv_opts, sizeof(drv_opts));
	if (ret) {
		SPDK_ERRLOG("Failed to set NVMe transport opts.\n");
		return ret;
	}

#define SET_FIELD(field, defval) \
		g_opts.field = SPDK_GET_FIELD(opts, field, defval, opts->opts_size); \

	SET_FIELD(action_on_timeout, 0);
	SET_FIELD(keep_alive_timeout_ms, 0);
	SET_FIELD(timeout_us, 0);
	SET_FIELD(timeout_admin_us, 0);
	SET_FIELD(transport_retry_count, 0);
	SET_FIELD(arbitration_burst, 0);
	SET_FIELD(low_priority_weight, 0);
	SET_FIELD(medium_priority_weight, 0);
	SET_FIELD(high_priority_weight, 0);
	SET_FIELD(io_queue_requests, 0);
	SET_FIELD(nvme_adminq_poll_period_us, 0);
	SET_FIELD(nvme_ioq_poll_period_us, 0);
	SET_FIELD(delay_cmd_submit, 0);
	SET_FIELD(bdev_retry_count, 0);
	SET_FIELD(ctrlr_loss_timeout_sec, 0);
	SET_FIELD(reconnect_delay_sec, 0);
	SET_FIELD(fast_io_fail_timeout_sec, 0);
	SET_FIELD(transport_ack_timeout, 0);
	SET_FIELD(disable_auto_failback, false);
	SET_FIELD(generate_uuids, false);
	SET_FIELD(transport_tos, 0);
	SET_FIELD(nvme_error_stat, false);
	SET_FIELD(io_path_stat, false);
	SET_FIELD(allow_accel_sequence, false);
	SET_FIELD(rdma_srq_size, 0);
	SET_FIELD(rdma_max_cq_size, 0);
	SET_FIELD(rdma_cm_event_timeout_ms, 0);
	SET_FIELD(dhchap_digests, 0);
	SET_FIELD(dhchap_dhgroups, 0);
	SET_FIELD(tcp_connect_timeout_ms, 0);
	SET_FIELD(enable_flush, false);

	g_opts.opts_size = opts->opts_size;

#undef SET_FIELD

	return 0;
}

/*
 * [한국어]
 * bdev_nvme_set_hotplug - bdev_nvme.h §2 참조. PCIe 핫플러그 폴링 활성화/비활성화.
 *
 * - enabled=true: bdev_nvme_hotplug poller 등록 (period_us마다 PCIe 스캔으로 새 SSD 감지).
 * - enabled=false: 같은 period로 bdev_nvme_remove_poller만 동작 (제거 처리).
 * Primary process만 hotplug enable 가능 (DPDK secondary는 PCIe 점유 못 함 → -EPERM).
 * 컨텍스트: app 스레드 강제.
 */
int
bdev_nvme_set_hotplug(bool enabled, uint64_t period_us)
{
	assert(spdk_thread_is_app_thread(NULL));

	if (enabled == true && !spdk_process_is_primary()) {
		return -EPERM;
	}

	period_us = period_us == 0 ? NVME_HOTPLUG_POLL_PERIOD_DEFAULT : period_us;
	period_us = spdk_min(period_us, NVME_HOTPLUG_POLL_PERIOD_MAX);

	spdk_poller_unregister(&g_hotplug_poller);
	if (enabled) {
		g_hotplug_poller = SPDK_POLLER_REGISTER(bdev_nvme_hotplug, NULL, period_us);
	} else {
		g_hotplug_poller = SPDK_POLLER_REGISTER(bdev_nvme_remove_poller, NULL,
							NVME_HOTPLUG_POLL_PERIOD_DEFAULT);
	}

	g_nvme_hotplug_poll_period_us = period_us;
	g_nvme_hotplug_enabled = enabled;
	return 0;
}

static void
nvme_ctrlr_populate_namespaces_done(struct nvme_ctrlr *nvme_ctrlr,
				    struct nvme_async_probe_ctx *ctx)
{
	struct nvme_ns	*nvme_ns;
	struct nvme_bdev	*nvme_bdev;
	size_t			j;

	assert(nvme_ctrlr != NULL);
	assert(spdk_thread_is_app_thread(NULL));

	if (ctx->names == NULL) {
		ctx->reported_bdevs = 0;
		populate_namespaces_cb(ctx, 0);
		return;
	}

	/*
	 * Report the new bdevs that were created in this call.
	 * There can be more than one bdev per NVMe controller.
	 */
	j = 0;

	RB_FOREACH(nvme_ns, nvme_ns_tree, &nvme_ctrlr->namespaces) {
		nvme_bdev = nvme_ns->bdev;
		if (j < ctx->max_bdevs) {
			ctx->names[j] = nvme_bdev->disk.name;
			j++;
		} else {

			NVME_CTRLR_ERRLOG(nvme_ctrlr,
					  "Maximum number of namespaces supported per NVMe controller is %du. "
					  "Unable to return all names of created bdevs\n",
					  ctx->max_bdevs);
			ctx->reported_bdevs = 0;
			populate_namespaces_cb(ctx, -ERANGE);
			return;
		}
	}

	ctx->reported_bdevs = j;
	populate_namespaces_cb(ctx, 0);
}

static int
bdev_nvme_check_secondary_trid(struct nvme_ctrlr *nvme_ctrlr,
			       struct spdk_nvme_ctrlr *new_ctrlr,
			       struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_path_id *tmp_trid;

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		NVME_CTRLR_ERRLOG(nvme_ctrlr, "PCIe failover is not supported.\n");
		return -ENOTSUP;
	}

	/* Currently we only support failover to the same transport type. */
	if (nvme_ctrlr->active_path_id->trid.trtype != trid->trtype) {
		NVME_CTRLR_WARNLOG(nvme_ctrlr,
				   "Failover from trtype: %s to a different trtype: %s is not supported currently\n",
				   spdk_nvme_transport_id_trtype_str(nvme_ctrlr->active_path_id->trid.trtype),
				   spdk_nvme_transport_id_trtype_str(trid->trtype));
		return -EINVAL;
	}


	/* Currently we only support failover to the same NQN. */
	if (strncmp(trid->subnqn, nvme_ctrlr->active_path_id->trid.subnqn, SPDK_NVMF_NQN_MAX_LEN)) {
		NVME_CTRLR_WARNLOG(nvme_ctrlr, "Failover to a different subnqn: %s is not supported currently\n",
				   trid->subnqn);
		return -EINVAL;
	}

	/* Skip all the other checks if we've already registered this path. */
	TAILQ_FOREACH(tmp_trid, &nvme_ctrlr->trids, link) {
		if (!spdk_nvme_transport_id_compare(&tmp_trid->trid, trid)) {
			NVME_CTRLR_WARNLOG(nvme_ctrlr, "This path is already registered\n");
			return -EALREADY;
		}
	}

	return 0;
}

static int
bdev_nvme_check_secondary_namespace(struct nvme_ctrlr *nvme_ctrlr,
				    struct spdk_nvme_ctrlr *new_ctrlr)
{
	struct nvme_ns *nvme_ns;
	struct spdk_nvme_ns *new_ns;

	assert(spdk_thread_is_app_thread(NULL));

	RB_FOREACH(nvme_ns, nvme_ns_tree, &nvme_ctrlr->namespaces) {
		new_ns = spdk_nvme_ctrlr_get_ns(new_ctrlr, nvme_ns->id);
		assert(new_ns != NULL);

		if (!bdev_nvme_compare_ns(nvme_ns->ns, new_ns)) {
			return -EINVAL;
		}
	}

	return 0;
}

static int
_bdev_nvme_add_secondary_trid(struct nvme_ctrlr *nvme_ctrlr,
			      struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_path_id *active_id, *new_trid, *tmp_trid;

	new_trid = calloc(1, sizeof(*new_trid));
	if (new_trid == NULL) {
		return -ENOMEM;
	}
	new_trid->trid = *trid;

	active_id = nvme_ctrlr->active_path_id;
	assert(active_id != NULL);
	assert(active_id == TAILQ_FIRST(&nvme_ctrlr->trids));

	/* Skip the active trid not to replace it until it is failed. */
	tmp_trid = TAILQ_NEXT(active_id, link);
	if (tmp_trid == NULL) {
		goto add_tail;
	}

	/* It means the trid is faled if its last failed time is non-zero.
	 * Insert the new alternate trid before any failed trid.
	 */
	TAILQ_FOREACH_FROM(tmp_trid, &nvme_ctrlr->trids, link) {
		if (tmp_trid->last_failed_tsc != 0) {
			TAILQ_INSERT_BEFORE(tmp_trid, new_trid, link);
			return 0;
		}
	}

add_tail:
	TAILQ_INSERT_TAIL(&nvme_ctrlr->trids, new_trid, link);
	return 0;
}

/* This is the case that a secondary path is added to an existing
 * nvme_ctrlr for failover. After checking if it can access the same
 * namespaces as the primary path, it is disconnected until failover occurs.
 */
static int
bdev_nvme_add_secondary_trid(struct nvme_ctrlr *nvme_ctrlr,
			     struct spdk_nvme_ctrlr *new_ctrlr,
			     struct spdk_nvme_transport_id *trid)
{
	int rc;

	assert(nvme_ctrlr != NULL);

	pthread_mutex_lock(&nvme_ctrlr->mutex);

	rc = bdev_nvme_check_secondary_trid(nvme_ctrlr, new_ctrlr, trid);
	if (rc != 0) {
		goto exit;
	}

	rc = bdev_nvme_check_secondary_namespace(nvme_ctrlr, new_ctrlr);
	if (rc != 0) {
		goto exit;
	}

	rc = _bdev_nvme_add_secondary_trid(nvme_ctrlr, trid);

exit:
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	spdk_nvme_detach(new_ctrlr);

	return rc;
}

static void
connect_attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ctrlr_opts *user_opts = cb_ctx;
	struct nvme_async_probe_ctx *ctx;
	int rc;

	ctx = SPDK_CONTAINEROF(user_opts, struct nvme_async_probe_ctx, drv_opts);
	ctx->ctrlr_attached = true;

	rc = nvme_ctrlr_create(ctrlr, ctx->base_name, &ctx->trid, ctx);
	if (rc != 0) {
		ctx->reported_bdevs = 0;
		populate_namespaces_cb(ctx, rc);
	}
}


static void
connect_set_failover_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
			struct spdk_nvme_ctrlr *ctrlr,
			const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ctrlr_opts *user_opts = cb_ctx;
	struct nvme_ctrlr *nvme_ctrlr;
	struct nvme_async_probe_ctx *ctx;
	int rc;

	ctx = SPDK_CONTAINEROF(user_opts, struct nvme_async_probe_ctx, drv_opts);
	ctx->ctrlr_attached = true;

	nvme_ctrlr = nvme_ctrlr_get_by_name(ctx->base_name);
	if (nvme_ctrlr) {
		rc = bdev_nvme_add_secondary_trid(nvme_ctrlr, ctrlr, &ctx->trid);
	} else {
		rc = -ENODEV;
	}

	ctx->reported_bdevs = 0;
	populate_namespaces_cb(ctx, rc);
}

static int
bdev_nvme_async_poll(void *arg)
{
	struct nvme_async_probe_ctx	*ctx = arg;
	int				rc;

	rc = spdk_nvme_probe_poll_async(ctx->probe_ctx);
	if (spdk_unlikely(rc != -EAGAIN)) {
		ctx->probe_done = true;
		spdk_poller_unregister(&ctx->poller);
		if (!ctx->ctrlr_attached) {
			/* The probe is done, but no controller was attached.
			 * That means we had a failure, so report -EIO back to
			 * the caller (usually the RPC). populate_namespaces_cb()
			 * will take care of freeing the nvme_async_probe_ctx.
			 */
			ctx->reported_bdevs = 0;
			populate_namespaces_cb(ctx, -EIO);
		} else if (ctx->namespaces_populated) {
			/* The namespaces for the attached controller were all
			 * populated and the response was already sent to the
			 * caller (usually the RPC).  So free the context here.
			 */
			free_nvme_async_probe_ctx(ctx);
		}
	}

	return SPDK_POLLER_BUSY;
}

static bool
bdev_nvme_check_io_error_resiliency_params(int32_t ctrlr_loss_timeout_sec,
		uint32_t reconnect_delay_sec,
		uint32_t fast_io_fail_timeout_sec)
{
	if (ctrlr_loss_timeout_sec < -1) {
		SPDK_ERRLOG("ctrlr_loss_timeout_sec can't be less than -1.\n");
		return false;
	} else if (ctrlr_loss_timeout_sec == -1) {
		if (reconnect_delay_sec == 0) {
			SPDK_ERRLOG("reconnect_delay_sec can't be 0 if ctrlr_loss_timeout_sec is not 0.\n");
			return false;
		} else if (fast_io_fail_timeout_sec != 0 &&
			   fast_io_fail_timeout_sec < reconnect_delay_sec) {
			SPDK_ERRLOG("reconnect_delay_sec can't be more than fast_io-fail_timeout_sec.\n");
			return false;
		}
	} else if (ctrlr_loss_timeout_sec != 0) {
		if (reconnect_delay_sec == 0) {
			SPDK_ERRLOG("reconnect_delay_sec can't be 0 if ctrlr_loss_timeout_sec is not 0.\n");
			return false;
		} else if (reconnect_delay_sec > (uint32_t)ctrlr_loss_timeout_sec) {
			SPDK_ERRLOG("reconnect_delay_sec can't be more than ctrlr_loss_timeout_sec.\n");
			return false;
		} else if (fast_io_fail_timeout_sec != 0) {
			if (fast_io_fail_timeout_sec < reconnect_delay_sec) {
				SPDK_ERRLOG("reconnect_delay_sec can't be more than fast_io_fail_timeout_sec.\n");
				return false;
			} else if (fast_io_fail_timeout_sec > (uint32_t)ctrlr_loss_timeout_sec) {
				SPDK_ERRLOG("fast_io_fail_timeout_sec can't be more than ctrlr_loss_timeout_sec.\n");
				return false;
			}
		}
	} else if (reconnect_delay_sec != 0 || fast_io_fail_timeout_sec != 0) {
		SPDK_ERRLOG("Both reconnect_delay_sec and fast_io_fail_timeout_sec must be 0 if ctrlr_loss_timeout_sec is 0.\n");
		return false;
	}

	return true;
}

int
spdk_bdev_nvme_create(struct spdk_nvme_transport_id *trid,
		      const char *base_name,
		      const char **names,
		      uint32_t count,
		      spdk_bdev_nvme_create_cb cb_fn,
		      void *cb_ctx,
		      struct spdk_nvme_ctrlr_opts *drv_opts,
		      struct spdk_bdev_nvme_ctrlr_opts *bdev_opts)
{
	struct nvme_probe_skip_entry *entry, *tmp;
	struct nvme_async_probe_ctx *ctx;
	spdk_nvme_attach_cb attach_cb;
	struct nvme_ctrlr *nvme_ctrlr;
	int len;

	/* TODO expand this check to include both the host and target TRIDs.
	 * Only if both are the same should we fail.
	 */
	if (nvme_ctrlr_get(trid, drv_opts->hostnqn) != NULL) {
		SPDK_ERRLOG("A controller with the provided trid (traddr: %s, hostnqn: %s) "
			    "already exists.\n", trid->traddr, drv_opts->hostnqn);
		return -EEXIST;
	}

	len = strnlen(base_name, SPDK_CONTROLLER_NAME_MAX);

	if (len == 0 || len == SPDK_CONTROLLER_NAME_MAX) {
		SPDK_ERRLOG("controller name must be between 1 and %d characters\n", SPDK_CONTROLLER_NAME_MAX - 1);
		return -EINVAL;
	}

	if (bdev_opts != NULL &&
	    !bdev_nvme_check_io_error_resiliency_params(bdev_opts->ctrlr_loss_timeout_sec,
			    bdev_opts->reconnect_delay_sec,
			    bdev_opts->fast_io_fail_timeout_sec)) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}
	ctx->base_name = strdup(base_name);
	if (!ctx->base_name) {
		free(ctx);
		return -ENOMEM;
	}
	ctx->names = names;
	ctx->max_bdevs = count;
	ctx->cb_fn = cb_fn;
	ctx->cb_ctx = cb_ctx;
	ctx->trid = *trid;

	if (bdev_opts) {
		memcpy(&ctx->bdev_opts, bdev_opts, sizeof(*bdev_opts));
	} else {
		spdk_bdev_nvme_get_default_ctrlr_opts(&ctx->bdev_opts);
	}

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		TAILQ_FOREACH_SAFE(entry, &g_skipped_nvme_ctrlrs, tailq, tmp) {
			if (spdk_nvme_transport_id_compare(trid, &entry->trid) == 0) {
				TAILQ_REMOVE(&g_skipped_nvme_ctrlrs, entry, tailq);
				free(entry);
				break;
			}
		}
	}

	memcpy(&ctx->drv_opts, drv_opts, sizeof(*drv_opts));
	ctx->drv_opts.transport_retry_count = g_opts.transport_retry_count;
	ctx->drv_opts.transport_ack_timeout = g_opts.transport_ack_timeout;
	ctx->drv_opts.keep_alive_timeout_ms = g_opts.keep_alive_timeout_ms;
	ctx->drv_opts.disable_read_ana_log_page = true;
	ctx->drv_opts.transport_tos = g_opts.transport_tos;

	if (spdk_interrupt_mode_is_enabled()) {
		if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
			ctx->drv_opts.enable_interrupts = true;
		} else {
			SPDK_ERRLOG("Interrupt mode is only supported with PCIe transport\n");
			free_nvme_async_probe_ctx(ctx);
			return -ENOTSUP;
		}
	}

	if (ctx->bdev_opts.psk != NULL) {
		ctx->drv_opts.tls_psk = spdk_keyring_get_key(ctx->bdev_opts.psk);
		if (ctx->drv_opts.tls_psk == NULL) {
			SPDK_ERRLOG("Could not load PSK: %s\n", ctx->bdev_opts.psk);
			free_nvme_async_probe_ctx(ctx);
			return -ENOKEY;
		}
	}

	if (ctx->bdev_opts.dhchap_key != NULL) {
		ctx->drv_opts.dhchap_key = spdk_keyring_get_key(ctx->bdev_opts.dhchap_key);
		if (ctx->drv_opts.dhchap_key == NULL) {
			SPDK_ERRLOG("Could not load DH-HMAC-CHAP key: %s\n",
				    ctx->bdev_opts.dhchap_key);
			free_nvme_async_probe_ctx(ctx);
			return -ENOKEY;
		}

		ctx->drv_opts.dhchap_digests = g_opts.dhchap_digests;
		ctx->drv_opts.dhchap_dhgroups = g_opts.dhchap_dhgroups;
	}
	if (ctx->bdev_opts.dhchap_ctrlr_key != NULL) {
		ctx->drv_opts.dhchap_ctrlr_key =
			spdk_keyring_get_key(ctx->bdev_opts.dhchap_ctrlr_key);
		if (ctx->drv_opts.dhchap_ctrlr_key == NULL) {
			SPDK_ERRLOG("Could not load DH-HMAC-CHAP controller key: %s\n",
				    ctx->bdev_opts.dhchap_ctrlr_key);
			free_nvme_async_probe_ctx(ctx);
			return -ENOKEY;
		}
	}

	if (nvme_bdev_ctrlr_get_by_name(base_name) == NULL || ctx->bdev_opts.multipath) {
		attach_cb = connect_attach_cb;
	} else {
		attach_cb = connect_set_failover_cb;
	}

	nvme_ctrlr = nvme_ctrlr_get_by_name(ctx->base_name);
	if (nvme_ctrlr  && nvme_ctrlr->opts.multipath != ctx->bdev_opts.multipath) {
		/* All controllers with the same name must be configured the same
		 * way, either for multipath or failover. If the configuration doesn't
		 * match - report error.
		 */
		free_nvme_async_probe_ctx(ctx);
		return -EINVAL;
	}

	ctx->probe_ctx = spdk_nvme_connect_async(trid, &ctx->drv_opts, attach_cb);
	if (ctx->probe_ctx == NULL) {
		SPDK_ERRLOG("No controller was found with provided trid (traddr: %s)\n", trid->traddr);
		free_nvme_async_probe_ctx(ctx);
		return -ENODEV;
	}
	ctx->poller = SPDK_POLLER_REGISTER(bdev_nvme_async_poll, ctx, 1000);

	return 0;
}

struct bdev_nvme_delete_ctx {
	char                        *name;
	struct spdk_nvme_path_id    path_id;
	spdk_bdev_nvme_delete_cb    delete_cb;
	void                        *delete_cb_ctx;
	uint64_t                    timeout_ticks;
	struct spdk_poller          *poller;
};

static void
free_bdev_nvme_delete_ctx(struct bdev_nvme_delete_ctx *ctx)
{
	if (ctx != NULL) {
		free(ctx->name);
		free(ctx);
	}
}

static bool
nvme_path_id_compare(struct spdk_nvme_path_id *p, const struct spdk_nvme_path_id *path_id)
{
	if (path_id->trid.trtype != 0) {
		if (path_id->trid.trtype == SPDK_NVME_TRANSPORT_CUSTOM) {
			if (strcasecmp(path_id->trid.trstring, p->trid.trstring) != 0) {
				return false;
			}
		} else {
			if (path_id->trid.trtype != p->trid.trtype) {
				return false;
			}
		}
	}

	if (!spdk_mem_all_zero(path_id->trid.traddr, sizeof(path_id->trid.traddr))) {
		if (strcasecmp(path_id->trid.traddr, p->trid.traddr) != 0) {
			return false;
		}
	}

	if (path_id->trid.adrfam != 0) {
		if (path_id->trid.adrfam != p->trid.adrfam) {
			return false;
		}
	}

	if (!spdk_mem_all_zero(path_id->trid.trsvcid, sizeof(path_id->trid.trsvcid))) {
		if (strcasecmp(path_id->trid.trsvcid, p->trid.trsvcid) != 0) {
			return false;
		}
	}

	if (!spdk_mem_all_zero(path_id->trid.subnqn, sizeof(path_id->trid.subnqn))) {
		if (strcmp(path_id->trid.subnqn, p->trid.subnqn) != 0) {
			return false;
		}
	}

	if (!spdk_mem_all_zero(path_id->hostid.hostaddr, sizeof(path_id->hostid.hostaddr))) {
		if (strcmp(path_id->hostid.hostaddr, p->hostid.hostaddr) != 0) {
			return false;
		}
	}

	if (!spdk_mem_all_zero(path_id->hostid.hostsvcid, sizeof(path_id->hostid.hostsvcid))) {
		if (strcmp(path_id->hostid.hostsvcid, p->hostid.hostsvcid) != 0) {
			return false;
		}
	}

	return true;
}

static bool
nvme_path_id_exists(const char *name, const struct spdk_nvme_path_id *path_id)
{
	struct nvme_bdev_ctrlr  *nbdev_ctrlr;
	struct nvme_ctrlr       *ctrlr;
	struct spdk_nvme_path_id     *p;

	assert(spdk_thread_is_app_thread(NULL));

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (!nbdev_ctrlr) {
		return false;
	}

	TAILQ_FOREACH(ctrlr, &nbdev_ctrlr->ctrlrs, tailq) {
		pthread_mutex_lock(&ctrlr->mutex);
		TAILQ_FOREACH(p, &ctrlr->trids, link) {
			if (nvme_path_id_compare(p, path_id)) {
				pthread_mutex_unlock(&ctrlr->mutex);
				return true;
			}
		}
		pthread_mutex_unlock(&ctrlr->mutex);
	}

	return false;
}

static int
bdev_nvme_delete_complete_poll(void *arg)
{
	struct bdev_nvme_delete_ctx     *ctx = arg;
	int                             rc = 0;

	if (nvme_path_id_exists(ctx->name, &ctx->path_id)) {
		if (ctx->timeout_ticks > spdk_get_ticks()) {
			return SPDK_POLLER_BUSY;
		}

		SPDK_ERRLOG("NVMe path '%s' still exists after delete\n", ctx->name);
		rc = -ETIMEDOUT;
	}

	spdk_poller_unregister(&ctx->poller);

	ctx->delete_cb(ctx->delete_cb_ctx, rc);
	free_bdev_nvme_delete_ctx(ctx);

	return SPDK_POLLER_BUSY;
}

static int
_bdev_nvme_delete(struct nvme_ctrlr *nvme_ctrlr, const struct spdk_nvme_path_id *path_id)
{
	struct spdk_nvme_path_id	*p, *t;
	spdk_msg_fn		msg_fn;
	int			rc = -ENXIO;

	pthread_mutex_lock(&nvme_ctrlr->mutex);

	TAILQ_FOREACH_REVERSE_SAFE(p, &nvme_ctrlr->trids, nvme_paths, link, t) {
		if (p == TAILQ_FIRST(&nvme_ctrlr->trids)) {
			break;
		}

		if (!nvme_path_id_compare(p, path_id)) {
			continue;
		}

		/* We are not using the specified path. */
		TAILQ_REMOVE(&nvme_ctrlr->trids, p, link);
		free(p);
		rc = 0;
	}

	if (p == NULL || !nvme_path_id_compare(p, path_id)) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return rc;
	}

	/* If we made it here, then this path is a match! Now we need to remove it. */

	/* This is the active path in use right now. The active path is always the first in the list. */
	assert(p == nvme_ctrlr->active_path_id);

	if (!TAILQ_NEXT(p, link)) {
		/* The current path is the only path. */
		msg_fn = _nvme_ctrlr_destruct;
		rc = bdev_nvme_delete_ctrlr_unsafe(nvme_ctrlr, false);
	} else {
		/* There is an alternative path. */
		msg_fn = _bdev_nvme_reset_ctrlr;
		rc = bdev_nvme_failover_ctrlr_unsafe(nvme_ctrlr, true);
	}

	if (rc == 0) {
		msg_fn(nvme_ctrlr);
	} else if (rc == -EALREADY) {
		rc = 0;
	}

	pthread_mutex_unlock(&nvme_ctrlr->mutex);
	return rc;
}

int
spdk_bdev_nvme_delete(const char *name, const struct spdk_nvme_path_id *path_id,
		      spdk_bdev_nvme_delete_cb delete_cb, void *cb_ctx)
{
	struct nvme_bdev_ctrlr		*nbdev_ctrlr;
	struct nvme_ctrlr		*nvme_ctrlr, *tmp_nvme_ctrlr;
	struct bdev_nvme_delete_ctx     *ctx = NULL;
	int				rc = -ENXIO, _rc;

	assert(spdk_thread_is_app_thread(NULL));

	if (name == NULL || path_id == NULL) {
		rc = -EINVAL;
		goto exit;
	}

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (nbdev_ctrlr == NULL) {
		SPDK_ERRLOG("Failed to find NVMe bdev controller\n");
		rc = -ENODEV;
		goto exit;
	}

	TAILQ_FOREACH_SAFE(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq, tmp_nvme_ctrlr) {
		_rc = _bdev_nvme_delete(nvme_ctrlr, path_id);
		if (_rc < 0 && _rc != -ENXIO) {
			rc = _rc;
			goto exit;
		} else if (_rc == 0) {
			/* We traverse all remaining nvme_ctrlrs even if one nvme_ctrlr
			 * was deleted successfully. To remember the successful deletion,
			 * overwrite rc only if _rc is zero.
			 */
			rc = 0;
		}
	}

	if (rc != 0 || delete_cb == NULL) {
		goto exit;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate context for bdev_nvme_delete\n");
		rc = -ENOMEM;
		goto exit;
	}

	ctx->name = strdup(name);
	if (ctx->name == NULL) {
		SPDK_ERRLOG("Failed to copy controller name for deletion\n");
		rc = -ENOMEM;
		goto exit;
	}

	ctx->delete_cb = delete_cb;
	ctx->delete_cb_ctx = cb_ctx;
	ctx->path_id = *path_id;
	ctx->timeout_ticks = spdk_get_ticks() + 10 * spdk_get_ticks_hz();
	ctx->poller = SPDK_POLLER_REGISTER(bdev_nvme_delete_complete_poll, ctx, 1000);
	if (ctx->poller == NULL) {
		SPDK_ERRLOG("Failed to register bdev_nvme_delete poller\n");
		rc = -ENOMEM;
		goto exit;
	}

exit:
	if (rc != 0) {
		free_bdev_nvme_delete_ctx(ctx);
	}

	return rc;
}

#define DISCOVERY_INFOLOG(ctx, format, ...) \
	SPDK_INFOLOG(bdev_nvme, "Discovery[%s:%s] " format, ctx->trid.traddr, ctx->trid.trsvcid, ##__VA_ARGS__);

#define DISCOVERY_ERRLOG(ctx, format, ...) \
	SPDK_ERRLOG("Discovery[%s:%s] " format, ctx->trid.traddr, ctx->trid.trsvcid, ##__VA_ARGS__);

struct discovery_entry_ctx {
	char						name[128];
	struct spdk_nvme_transport_id			trid;
	struct spdk_nvme_ctrlr_opts			drv_opts;
	struct spdk_nvmf_discovery_log_page_entry	entry;
	TAILQ_ENTRY(discovery_entry_ctx)		tailq;
	struct discovery_ctx				*ctx;
};

struct discovery_ctx {
	char					*name;
	spdk_bdev_nvme_start_discovery_fn	start_cb_fn;
	spdk_bdev_nvme_stop_discovery_fn	stop_cb_fn;
	void					*cb_ctx;
	struct spdk_nvme_probe_ctx		*probe_ctx;
	struct spdk_nvme_detach_ctx		*detach_ctx;
	struct spdk_nvme_ctrlr			*ctrlr;
	struct spdk_nvme_transport_id		trid;
	struct discovery_entry_ctx		*entry_ctx_in_use;
	struct spdk_poller			*poller;
	struct spdk_nvme_ctrlr_opts		drv_opts;
	struct spdk_bdev_nvme_ctrlr_opts	bdev_opts;
	struct spdk_nvmf_discovery_log_page	*log_page;
	TAILQ_ENTRY(discovery_ctx)		tailq;
	TAILQ_HEAD(, discovery_entry_ctx)	nvm_entry_ctxs;
	TAILQ_HEAD(, discovery_entry_ctx)	discovery_entry_ctxs;
	int					rc;
	bool					wait_for_attach;
	uint64_t				timeout_ticks;
	/* Denotes that the discovery service is being started. We're waiting
	 * for the initial connection to the discovery controller to be
	 * established and attach discovered NVM ctrlrs.
	 */
	bool					initializing;
	/* Denotes if a discovery is currently in progress for this context.
	 * That includes connecting to newly discovered subsystems.  Used to
	 * ensure we do not start a new discovery until an existing one is
	 * complete.
	 */
	bool					in_progress;

	/* Denotes if another discovery is needed after the one in progress
	 * completes.  Set when we receive an AER completion while a discovery
	 * is already in progress.
	 */
	bool					pending;

	/* Signal to the discovery context poller that it should stop the
	 * discovery service, including detaching from the current discovery
	 * controller.
	 */
	bool					stop;

	uint32_t				index;
	uint32_t				attach_in_progress;
	char					*hostnqn;

	/* Denotes if the discovery service was started by the mdns discovery.
	 */
	bool					from_mdns_discovery_service;
};

TAILQ_HEAD(discovery_ctxs, discovery_ctx);
static struct discovery_ctxs g_discovery_ctxs = TAILQ_HEAD_INITIALIZER(g_discovery_ctxs);

static void get_discovery_log_page(struct discovery_ctx *ctx);

static void
free_discovery_ctx(struct discovery_ctx *ctx)
{
	free(ctx->log_page);
	free(ctx->hostnqn);
	free(ctx->name);
	free(ctx);
}

static void
discovery_complete(struct discovery_ctx *ctx)
{
	ctx->initializing = false;
	ctx->in_progress = false;
	if (ctx->pending) {
		ctx->pending = false;
		get_discovery_log_page(ctx);
	}
}

static void
build_trid_from_log_page_entry(struct spdk_nvme_transport_id *trid,
			       struct spdk_nvmf_discovery_log_page_entry *entry)
{
	char *space;

	trid->trtype = entry->trtype;
	trid->adrfam = entry->adrfam;
	memcpy(trid->traddr, entry->traddr, sizeof(entry->traddr));
	memcpy(trid->trsvcid, entry->trsvcid, sizeof(entry->trsvcid));
	/* Because the source buffer (entry->subnqn) is longer than trid->subnqn, and
	 * before call to this function trid->subnqn is zeroed out, we need
	 * to copy sizeof(trid->subnqn) minus one byte to make sure the last character
	 * remains 0. Then we can shorten the string (replace ' ' with 0) if required
	 */
	memcpy(trid->subnqn, entry->subnqn, sizeof(trid->subnqn) - 1);

	/* We want the traddr, trsvcid and subnqn fields to be NULL-terminated.
	 * But the log page entries typically pad them with spaces, not zeroes.
	 * So add a NULL terminator to each of these fields at the appropriate
	 * location.
	 */
	space = strchr(trid->traddr, ' ');
	if (space) {
		*space = 0;
	}
	space = strchr(trid->trsvcid, ' ');
	if (space) {
		*space = 0;
	}
	space = strchr(trid->subnqn, ' ');
	if (space) {
		*space = 0;
	}
}

static void
_stop_discovery(void *_ctx)
{
	struct discovery_ctx *ctx = _ctx;

	if (ctx->attach_in_progress > 0) {
		spdk_thread_send_msg(spdk_get_thread(), _stop_discovery, ctx);
		return;
	}

	ctx->stop = true;

	while (!TAILQ_EMPTY(&ctx->nvm_entry_ctxs)) {
		struct discovery_entry_ctx *entry_ctx;
		struct spdk_nvme_path_id path = {};

		entry_ctx = TAILQ_FIRST(&ctx->nvm_entry_ctxs);
		path.trid = entry_ctx->trid;
		spdk_bdev_nvme_delete(entry_ctx->name, &path, NULL, NULL);
		TAILQ_REMOVE(&ctx->nvm_entry_ctxs, entry_ctx, tailq);
		free(entry_ctx);
	}

	while (!TAILQ_EMPTY(&ctx->discovery_entry_ctxs)) {
		struct discovery_entry_ctx *entry_ctx;

		entry_ctx = TAILQ_FIRST(&ctx->discovery_entry_ctxs);
		TAILQ_REMOVE(&ctx->discovery_entry_ctxs, entry_ctx, tailq);
		free(entry_ctx);
	}

	free(ctx->entry_ctx_in_use);
	ctx->entry_ctx_in_use = NULL;
}

static void
stop_discovery(struct discovery_ctx *ctx, spdk_bdev_nvme_stop_discovery_fn cb_fn, void *cb_ctx)
{
	ctx->stop_cb_fn = cb_fn;
	ctx->cb_ctx = cb_ctx;

	if (ctx->attach_in_progress > 0) {
		DISCOVERY_INFOLOG(ctx, "stopping discovery with attach_in_progress: %"PRIu32"\n",
				  ctx->attach_in_progress);
	}

	_stop_discovery(ctx);
}

static void
remove_discovery_entry(struct nvme_ctrlr *nvme_ctrlr)
{
	struct discovery_ctx *d_ctx;
	struct spdk_nvme_path_id *path_id;
	struct spdk_nvme_transport_id trid = {};
	struct discovery_entry_ctx *entry_ctx, *tmp;

	path_id = TAILQ_FIRST(&nvme_ctrlr->trids);

	TAILQ_FOREACH(d_ctx, &g_discovery_ctxs, tailq) {
		TAILQ_FOREACH_SAFE(entry_ctx, &d_ctx->nvm_entry_ctxs, tailq, tmp) {
			build_trid_from_log_page_entry(&trid, &entry_ctx->entry);
			if (spdk_nvme_transport_id_compare(&trid, &path_id->trid) != 0) {
				continue;
			}

			TAILQ_REMOVE(&d_ctx->nvm_entry_ctxs, entry_ctx, tailq);
			free(entry_ctx);
			DISCOVERY_INFOLOG(d_ctx, "Remove discovery entry: %s:%s:%s\n",
					  trid.subnqn, trid.traddr, trid.trsvcid);

			/* Fail discovery ctrlr to force reattach attempt */
			spdk_nvme_ctrlr_fail(d_ctx->ctrlr);
		}
	}
}

static void
discovery_remove_controllers(struct discovery_ctx *ctx)
{
	struct spdk_nvmf_discovery_log_page *log_page = ctx->log_page;
	struct discovery_entry_ctx *entry_ctx, *tmp;
	struct spdk_nvmf_discovery_log_page_entry *new_entry, *old_entry;
	struct spdk_nvme_transport_id old_trid = {};
	uint64_t numrec, i;
	bool found;

	numrec = from_le64(&log_page->numrec);
	TAILQ_FOREACH_SAFE(entry_ctx, &ctx->nvm_entry_ctxs, tailq, tmp) {
		found = false;
		old_entry = &entry_ctx->entry;
		build_trid_from_log_page_entry(&old_trid, old_entry);
		for (i = 0; i < numrec; i++) {
			new_entry = &log_page->entries[i];
			if (!memcmp(old_entry, new_entry, sizeof(*old_entry))) {
				DISCOVERY_INFOLOG(ctx, "NVM %s:%s:%s found again\n",
						  old_trid.subnqn, old_trid.traddr, old_trid.trsvcid);
				found = true;
				break;
			}
		}
		if (!found) {
			struct spdk_nvme_path_id path = {};

			DISCOVERY_INFOLOG(ctx, "NVM %s:%s:%s not found\n",
					  old_trid.subnqn, old_trid.traddr, old_trid.trsvcid);

			path.trid = entry_ctx->trid;
			spdk_bdev_nvme_delete(entry_ctx->name, &path, NULL, NULL);
			TAILQ_REMOVE(&ctx->nvm_entry_ctxs, entry_ctx, tailq);
			free(entry_ctx);
		}
	}
	free(log_page);
	ctx->log_page = NULL;
	discovery_complete(ctx);
}

static void
complete_discovery_start(struct discovery_ctx *ctx, int status)
{
	ctx->timeout_ticks = 0;
	ctx->rc = status;
	if (ctx->start_cb_fn) {
		ctx->start_cb_fn(ctx->cb_ctx, status);
		ctx->start_cb_fn = NULL;
		ctx->cb_ctx = NULL;
	}
}

static void
discovery_attach_controller_done(void *cb_ctx, size_t bdev_count, int rc)
{
	struct discovery_entry_ctx *entry_ctx = cb_ctx;
	struct discovery_ctx *ctx = entry_ctx->ctx;

	DISCOVERY_INFOLOG(ctx, "attach %s done\n", entry_ctx->name);
	ctx->attach_in_progress--;
	if (ctx->attach_in_progress == 0) {
		complete_discovery_start(ctx, ctx->rc);
		if (ctx->initializing && ctx->rc != 0) {
			DISCOVERY_ERRLOG(ctx, "stopping discovery due to errors: %d\n", ctx->rc);
			stop_discovery(ctx, NULL, ctx->cb_ctx);
		} else {
			discovery_remove_controllers(ctx);
		}
	}
}

static struct discovery_entry_ctx *
create_discovery_entry_ctx(struct discovery_ctx *ctx, struct spdk_nvme_transport_id *trid)
{
	struct discovery_entry_ctx *new_ctx;

	new_ctx = calloc(1, sizeof(*new_ctx));
	if (new_ctx == NULL) {
		DISCOVERY_ERRLOG(ctx, "could not allocate new entry_ctx\n");
		return NULL;
	}

	new_ctx->ctx = ctx;
	memcpy(&new_ctx->trid, trid, sizeof(*trid));
	spdk_nvme_ctrlr_get_default_ctrlr_opts(&new_ctx->drv_opts, sizeof(new_ctx->drv_opts));
	snprintf(new_ctx->drv_opts.hostnqn, sizeof(new_ctx->drv_opts.hostnqn), "%s", ctx->hostnqn);
	return new_ctx;
}

static void
discovery_log_page_cb(void *cb_arg, int rc, const struct spdk_nvme_cpl *cpl,
		      struct spdk_nvmf_discovery_log_page *log_page)
{
	struct discovery_ctx *ctx = cb_arg;
	struct discovery_entry_ctx *entry_ctx, *tmp;
	struct spdk_nvmf_discovery_log_page_entry *new_entry, *old_entry;
	uint64_t numrec, i;
	bool found;

	if (rc || spdk_nvme_cpl_is_error(cpl)) {
		DISCOVERY_ERRLOG(ctx, "could not get discovery log page\n");
		return;
	}

	ctx->log_page = log_page;
	assert(ctx->attach_in_progress == 0);
	numrec = from_le64(&log_page->numrec);
	TAILQ_FOREACH_SAFE(entry_ctx, &ctx->discovery_entry_ctxs, tailq, tmp) {
		TAILQ_REMOVE(&ctx->discovery_entry_ctxs, entry_ctx, tailq);
		free(entry_ctx);
	}
	for (i = 0; i < numrec; i++) {
		found = false;
		new_entry = &log_page->entries[i];
		if (new_entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT ||
		    new_entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
			struct discovery_entry_ctx *new_ctx;
			struct spdk_nvme_transport_id trid = {};

			build_trid_from_log_page_entry(&trid, new_entry);
			new_ctx = create_discovery_entry_ctx(ctx, &trid);
			if (new_ctx == NULL) {
				DISCOVERY_ERRLOG(ctx, "could not allocate new entry_ctx\n");
				break;
			}

			TAILQ_INSERT_TAIL(&ctx->discovery_entry_ctxs, new_ctx, tailq);
			continue;
		}
		TAILQ_FOREACH(entry_ctx, &ctx->nvm_entry_ctxs, tailq) {
			old_entry = &entry_ctx->entry;
			if (!memcmp(new_entry, old_entry, sizeof(*new_entry))) {
				found = true;
				break;
			}
		}
		if (!found) {
			struct discovery_entry_ctx *subnqn_ctx = NULL, *new_ctx;
			struct discovery_ctx *d_ctx;

			TAILQ_FOREACH(d_ctx, &g_discovery_ctxs, tailq) {
				TAILQ_FOREACH(subnqn_ctx, &d_ctx->nvm_entry_ctxs, tailq) {
					if (!memcmp(subnqn_ctx->entry.subnqn, new_entry->subnqn,
						    sizeof(new_entry->subnqn))) {
						break;
					}
				}
				if (subnqn_ctx) {
					break;
				}
			}

			new_ctx = calloc(1, sizeof(*new_ctx));
			if (new_ctx == NULL) {
				DISCOVERY_ERRLOG(ctx, "could not allocate new entry_ctx\n");
				break;
			}

			new_ctx->ctx = ctx;
			memcpy(&new_ctx->entry, new_entry, sizeof(*new_entry));
			build_trid_from_log_page_entry(&new_ctx->trid, new_entry);
			if (subnqn_ctx) {
				snprintf(new_ctx->name, sizeof(new_ctx->name), "%s", subnqn_ctx->name);
				DISCOVERY_INFOLOG(ctx, "NVM %s:%s:%s new path for %s\n",
						  new_ctx->trid.subnqn, new_ctx->trid.traddr, new_ctx->trid.trsvcid,
						  new_ctx->name);
			} else {
				snprintf(new_ctx->name, sizeof(new_ctx->name), "%s%d", ctx->name, ctx->index++);
				DISCOVERY_INFOLOG(ctx, "NVM %s:%s:%s new subsystem %s\n",
						  new_ctx->trid.subnqn, new_ctx->trid.traddr, new_ctx->trid.trsvcid,
						  new_ctx->name);
			}
			spdk_nvme_ctrlr_get_default_ctrlr_opts(&new_ctx->drv_opts, sizeof(new_ctx->drv_opts));
			snprintf(new_ctx->drv_opts.hostnqn, sizeof(new_ctx->drv_opts.hostnqn), "%s", ctx->hostnqn);
			rc = spdk_bdev_nvme_create(&new_ctx->trid, new_ctx->name, NULL, 0,
						   discovery_attach_controller_done, new_ctx,
						   &new_ctx->drv_opts, &ctx->bdev_opts);
			if (rc == 0) {
				TAILQ_INSERT_TAIL(&ctx->nvm_entry_ctxs, new_ctx, tailq);
				ctx->attach_in_progress++;
			} else {
				DISCOVERY_ERRLOG(ctx, "spdk_bdev_nvme_create failed (%s)\n", spdk_strerror(-rc));
			}
		}
	}

	if (ctx->attach_in_progress == 0) {
		discovery_remove_controllers(ctx);
	}
}

static void
get_discovery_log_page(struct discovery_ctx *ctx)
{
	int rc;

	assert(ctx->in_progress == false);
	ctx->in_progress = true;
	rc = spdk_nvme_ctrlr_get_discovery_log_page(ctx->ctrlr, discovery_log_page_cb, ctx);
	if (rc != 0) {
		DISCOVERY_ERRLOG(ctx, "could not get discovery log page\n");
	}
	DISCOVERY_INFOLOG(ctx, "sent discovery log page command\n");
}

static void
discovery_aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct discovery_ctx *ctx = arg;
	uint32_t log_page_id = (cpl->cdw0 & 0xFF0000) >> 16;

	if (spdk_nvme_cpl_is_error(cpl)) {
		DISCOVERY_ERRLOG(ctx, "aer failed\n");
		return;
	}

	if (log_page_id != SPDK_NVME_LOG_DISCOVERY) {
		DISCOVERY_ERRLOG(ctx, "unexpected log page 0x%x\n", log_page_id);
		return;
	}

	DISCOVERY_INFOLOG(ctx, "got aer\n");
	if (ctx->in_progress) {
		ctx->pending = true;
		return;
	}

	get_discovery_log_page(ctx);
}

static void
discovery_attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		    struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ctrlr_opts *user_opts = cb_ctx;
	struct discovery_ctx *ctx;

	ctx = SPDK_CONTAINEROF(user_opts, struct discovery_ctx, drv_opts);

	DISCOVERY_INFOLOG(ctx, "discovery ctrlr attached\n");
	ctx->probe_ctx = NULL;
	ctx->ctrlr = ctrlr;

	if (ctx->rc != 0) {
		DISCOVERY_ERRLOG(ctx, "encountered error while attaching discovery ctrlr: %d\n",
				 ctx->rc);
		return;
	}

	spdk_nvme_ctrlr_register_aer_callback(ctx->ctrlr, discovery_aer_cb, ctx);
}

static int
discovery_poller(void *arg)
{
	struct discovery_ctx *ctx = arg;
	struct spdk_nvme_transport_id *trid;
	int rc;

	if (ctx->detach_ctx) {
		rc = spdk_nvme_detach_poll_async(ctx->detach_ctx);
		if (rc != -EAGAIN) {
			ctx->detach_ctx = NULL;
			ctx->ctrlr = NULL;
		}
	} else if (ctx->stop) {
		if (ctx->ctrlr != NULL) {
			rc = spdk_nvme_detach_async(ctx->ctrlr, &ctx->detach_ctx);
			if (rc == 0) {
				return SPDK_POLLER_BUSY;
			}
			DISCOVERY_ERRLOG(ctx, "could not detach discovery ctrlr\n");
		}
		spdk_poller_unregister(&ctx->poller);
		TAILQ_REMOVE(&g_discovery_ctxs, ctx, tailq);
		assert(ctx->start_cb_fn == NULL);
		if (ctx->stop_cb_fn != NULL) {
			ctx->stop_cb_fn(ctx->cb_ctx);
		}
		free_discovery_ctx(ctx);
	} else if (ctx->probe_ctx == NULL && ctx->ctrlr == NULL) {
		if (ctx->timeout_ticks != 0 && ctx->timeout_ticks < spdk_get_ticks()) {
			DISCOVERY_ERRLOG(ctx, "timed out while attaching discovery ctrlr\n");
			assert(ctx->initializing);
			spdk_poller_unregister(&ctx->poller);
			TAILQ_REMOVE(&g_discovery_ctxs, ctx, tailq);
			complete_discovery_start(ctx, -ETIMEDOUT);
			stop_discovery(ctx, NULL, NULL);
			free_discovery_ctx(ctx);
			return SPDK_POLLER_BUSY;
		}

		assert(ctx->entry_ctx_in_use == NULL);
		ctx->entry_ctx_in_use = TAILQ_FIRST(&ctx->discovery_entry_ctxs);
		TAILQ_REMOVE(&ctx->discovery_entry_ctxs, ctx->entry_ctx_in_use, tailq);
		trid = &ctx->entry_ctx_in_use->trid;

		/* All controllers must be configured explicitely either for multipath or failover.
		 * While discovery use multipath mode, we need to set this in bdev options as well.
		 */
		ctx->bdev_opts.multipath = true;

		ctx->probe_ctx = spdk_nvme_connect_async(trid, &ctx->drv_opts, discovery_attach_cb);
		if (ctx->probe_ctx) {
			spdk_poller_unregister(&ctx->poller);
			ctx->poller = SPDK_POLLER_REGISTER(discovery_poller, ctx, 1000);
		} else {
			DISCOVERY_ERRLOG(ctx, "could not start discovery connect\n");
			TAILQ_INSERT_TAIL(&ctx->discovery_entry_ctxs, ctx->entry_ctx_in_use, tailq);
			ctx->entry_ctx_in_use = NULL;
		}
	} else if (ctx->probe_ctx) {
		if (ctx->timeout_ticks != 0 && ctx->timeout_ticks < spdk_get_ticks()) {
			DISCOVERY_ERRLOG(ctx, "timed out while attaching discovery ctrlr\n");
			complete_discovery_start(ctx, -ETIMEDOUT);
			return SPDK_POLLER_BUSY;
		}

		rc = spdk_nvme_probe_poll_async(ctx->probe_ctx);
		if (rc != -EAGAIN) {
			if (ctx->rc != 0) {
				assert(ctx->initializing);
				stop_discovery(ctx, NULL, ctx->cb_ctx);
			} else {
				assert(rc == 0);
				DISCOVERY_INFOLOG(ctx, "discovery ctrlr connected\n");
				ctx->rc = rc;
				get_discovery_log_page(ctx);
			}
		}
	} else {
		if (ctx->timeout_ticks != 0 && ctx->timeout_ticks < spdk_get_ticks()) {
			DISCOVERY_ERRLOG(ctx, "timed out while attaching NVM ctrlrs\n");
			complete_discovery_start(ctx, -ETIMEDOUT);
			/* We need to wait until all NVM ctrlrs are attached before we stop the
			 * discovery service to make sure we don't detach a ctrlr that is still
			 * being attached.
			 */
			if (ctx->attach_in_progress == 0) {
				stop_discovery(ctx, NULL, ctx->cb_ctx);
				return SPDK_POLLER_BUSY;
			}
		}

		rc = spdk_nvme_ctrlr_process_admin_completions(ctx->ctrlr);
		if (rc < 0) {
			spdk_poller_unregister(&ctx->poller);
			ctx->poller = SPDK_POLLER_REGISTER(discovery_poller, ctx, 1000 * 1000);
			TAILQ_INSERT_TAIL(&ctx->discovery_entry_ctxs, ctx->entry_ctx_in_use, tailq);
			ctx->entry_ctx_in_use = NULL;

			rc = spdk_nvme_detach_async(ctx->ctrlr, &ctx->detach_ctx);
			if (rc != 0) {
				DISCOVERY_ERRLOG(ctx, "could not detach discovery ctrlr\n");
				ctx->ctrlr = NULL;
			}
		}
	}

	return SPDK_POLLER_BUSY;
}

/*
 * [한국어]
 * bdev_nvme_start_discovery - bdev_nvme.h §2 참조. NVMe-oF 디스커버리 세션 시작.
 *
 * 동작:
 *   1) trid->subnqn에 표준 디스커버리 NQN(SPDK_NVMF_DISCOVERY_NQN) 강제.
 *   2) 같은 base_name이나 같은 trid가 이미 활성이면 -EEXIST.
 *   3) 새 discovery_ctx 할당, drv/bdev opts 사본, hostnqn strdup, entry_ctx 생성.
 *   4) 1초 주기 discovery_poller 등록 (Discovery Log Page 폴링하며 새 NVM 서브시스템 자동 attach).
 * 컨텍스트: app 스레드 강제.
 */
int
bdev_nvme_start_discovery(struct spdk_nvme_transport_id *trid,
			  const char *base_name,
			  struct spdk_nvme_ctrlr_opts *drv_opts,
			  struct spdk_bdev_nvme_ctrlr_opts *bdev_opts,
			  uint64_t attach_timeout,
			  bool from_mdns,
			  spdk_bdev_nvme_start_discovery_fn cb_fn, void *cb_ctx)
{
	struct discovery_ctx *ctx;
	struct discovery_entry_ctx *discovery_entry_ctx;

	assert(spdk_thread_is_app_thread(NULL));

	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	TAILQ_FOREACH(ctx, &g_discovery_ctxs, tailq) {
		if (strcmp(ctx->name, base_name) == 0) {
			return -EEXIST;
		}

		if (ctx->entry_ctx_in_use != NULL) {
			if (!spdk_nvme_transport_id_compare(trid, &ctx->entry_ctx_in_use->trid)) {
				return -EEXIST;
			}
		}

		TAILQ_FOREACH(discovery_entry_ctx, &ctx->discovery_entry_ctxs, tailq) {
			if (!spdk_nvme_transport_id_compare(trid, &discovery_entry_ctx->trid)) {
				return -EEXIST;
			}
		}
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->name = strdup(base_name);
	if (ctx->name == NULL) {
		free_discovery_ctx(ctx);
		return -ENOMEM;
	}
	memcpy(&ctx->drv_opts, drv_opts, sizeof(*drv_opts));
	memcpy(&ctx->bdev_opts, bdev_opts, sizeof(*bdev_opts));
	ctx->from_mdns_discovery_service = from_mdns;
	ctx->bdev_opts.from_discovery_service = true;
	ctx->start_cb_fn = cb_fn;
	ctx->cb_ctx = cb_ctx;
	ctx->initializing = true;
	if (ctx->start_cb_fn) {
		/* We can use this when dumping json to denote if this RPC parameter
		 * was specified or not.
		 */
		ctx->wait_for_attach = true;
	}
	if (attach_timeout != 0) {
		ctx->timeout_ticks = spdk_get_ticks() + attach_timeout *
				     spdk_get_ticks_hz() / 1000ull;
	}
	TAILQ_INIT(&ctx->nvm_entry_ctxs);
	TAILQ_INIT(&ctx->discovery_entry_ctxs);
	memcpy(&ctx->trid, trid, sizeof(*trid));
	/* Even if user did not specify hostnqn, we can still strdup("\0"); */
	ctx->hostnqn = strdup(ctx->drv_opts.hostnqn);
	if (ctx->hostnqn == NULL) {
		free_discovery_ctx(ctx);
		return -ENOMEM;
	}
	discovery_entry_ctx = create_discovery_entry_ctx(ctx, trid);
	if (discovery_entry_ctx == NULL) {
		DISCOVERY_ERRLOG(ctx, "could not allocate new entry_ctx\n");
		free_discovery_ctx(ctx);
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&ctx->discovery_entry_ctxs, discovery_entry_ctx, tailq);
	TAILQ_INSERT_TAIL(&g_discovery_ctxs, ctx, tailq);
	ctx->poller = SPDK_POLLER_REGISTER(discovery_poller, ctx, 1000 * 1000);
	return 0;
}

/*
 * [한국어]
 * bdev_nvme_stop_discovery - bdev_nvme.h §2 참조. 진행 중인 디스커버리 세션 중단.
 *
 * 이름으로 ctx lookup → stop_discovery에 위임 (디스커버리 컨트롤러 detach 비동기 시퀀스).
 * 이미 stop 중이거나 초기화 중 에러 상태면 -EALREADY.
 * 디스커버리로 attach된 컨트롤러들은 그대로 유지 (별도 detach 필요).
 */
int
bdev_nvme_stop_discovery(const char *name, spdk_bdev_nvme_stop_discovery_fn cb_fn, void *cb_ctx)
{
	struct discovery_ctx *ctx;

	TAILQ_FOREACH(ctx, &g_discovery_ctxs, tailq) {
		if (strcmp(name, ctx->name) == 0) {
			if (ctx->stop) {
				return -EALREADY;
			}
			/* If we're still starting the discovery service and ->rc is non-zero, we're
			 * going to stop it as soon as we can
			 */
			if (ctx->initializing && ctx->rc != 0) {
				return -EALREADY;
			}
			stop_discovery(ctx, cb_fn, cb_ctx);
			return 0;
		}
	}

	return -ENOENT;
}

static int
bdev_nvme_init(void)
{
	assert(spdk_thread_is_app_thread(NULL));

	spdk_io_device_register(&g_nvme_bdev_ctrlrs, bdev_nvme_create_poll_group_cb,
				bdev_nvme_destroy_poll_group_cb,
				sizeof(struct nvme_poll_group),  "nvme_poll_groups");

	g_bdev_nvme_init_done = true;
	return 0;
}

static void
bdev_nvme_fini_destruct_ctrlrs(void)
{
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;

	assert(spdk_thread_is_app_thread(NULL));

	TAILQ_FOREACH(nbdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		TAILQ_FOREACH(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq) {
			pthread_mutex_lock(&nvme_ctrlr->mutex);
			if (nvme_ctrlr->destruct) {
				/* This controller's destruction was already started
				 * before the application started shutting down
				 */
				pthread_mutex_unlock(&nvme_ctrlr->mutex);
				continue;
			}
			nvme_ctrlr->destruct = true;
			pthread_mutex_unlock(&nvme_ctrlr->mutex);

			_nvme_ctrlr_destruct(nvme_ctrlr);
		}
	}

	g_bdev_nvme_module_finish = true;
	bdev_nvme_fini_done();
}

static void
check_discovery_fini(void *arg)
{
	if (TAILQ_EMPTY(&g_discovery_ctxs)) {
		bdev_nvme_fini_destruct_ctrlrs();
	}
}

static void
bdev_nvme_fini(void)
{
	struct nvme_probe_skip_entry *entry, *entry_tmp;
	struct discovery_ctx *ctx;

	assert(spdk_thread_is_app_thread(NULL));

	spdk_poller_unregister(&g_hotplug_poller);
	free(g_hotplug_probe_ctx);
	g_hotplug_probe_ctx = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_skipped_nvme_ctrlrs, tailq, entry_tmp) {
		TAILQ_REMOVE(&g_skipped_nvme_ctrlrs, entry, tailq);
		free(entry);
	}

	if (TAILQ_EMPTY(&g_discovery_ctxs)) {
		bdev_nvme_fini_destruct_ctrlrs();
	} else {
		TAILQ_FOREACH(ctx, &g_discovery_ctxs, tailq) {
			stop_discovery(ctx, check_discovery_fini, NULL);
		}
	}
}

static void
bdev_nvme_verify_pi_error(struct nvme_bdev_io *bio)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk = {};
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = bdev->dif_pi_format;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen, bdev->md_len, bdev->md_interleave,
			       bdev->dif_is_head_of_md, bdev->dif_type,
			       bdev_io->u.bdev.dif_check_flags,
			       bdev_io->u.bdev.offset_blocks, 0, 0, 0, 0, &dif_opts);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF context failed\n");
		return;
	}

	if (bdev->md_interleave) {
		rc = spdk_dif_verify(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				     bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk);
	} else {
		struct iovec md_iov = {
			.iov_base	= bdev_io->u.bdev.md_buf,
			.iov_len	= bdev_io->u.bdev.num_blocks * bdev->md_len,
		};

		rc = spdk_dix_verify(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				     &md_iov, bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk);
	}

	if (rc != 0) {
		SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n",
			    err_blk.err_type, err_blk.err_offset);
	} else {
		SPDK_ERRLOG("Hardware reported PI error but SPDK could not find any.\n");
	}
}

static void
bdev_nvme_no_pi_readv_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	if (spdk_nvme_cpl_is_success(cpl)) {
		/* Run PI verification for read data buffer. */
		bdev_nvme_verify_pi_error(bio);
	}

	/* Return original completion status */
	bdev_nvme_io_complete_nvme_status(bio, &bio->cpl);
}

static void
bdev_nvme_readv_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	int ret;

	if (spdk_unlikely(spdk_nvme_cpl_is_pi_error(cpl))) {
		SPDK_ERRLOG("readv completed with PI error (sct=%d, sc=%d)\n",
			    cpl->status.sct, cpl->status.sc);

		/* Save completion status to use after verifying PI error. */
		bio->cpl = *cpl;

		if (spdk_likely(nvme_io_path_is_available(bio->io_path))) {
			/* Read without PI checking to verify PI error. */
			ret = bdev_nvme_no_pi_readv(bio,
						    bdev_io->u.bdev.iovs,
						    bdev_io->u.bdev.iovcnt,
						    bdev_io->u.bdev.md_buf,
						    bdev_io->u.bdev.num_blocks,
						    bdev_io->u.bdev.offset_blocks);
			if (ret == 0) {
				return;
			}
		}
	}

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static void
bdev_nvme_writev_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	if (spdk_unlikely(spdk_nvme_cpl_is_pi_error(cpl))) {
		SPDK_ERRLOG("writev completed with PI error (sct=%d, sc=%d)\n",
			    cpl->status.sct, cpl->status.sc);
		/* Run PI verification for write data buffer if PI error is detected. */
		bdev_nvme_verify_pi_error(bio);
	}

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static void
bdev_nvme_zone_appendv_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);

	/* spdk_bdev_io_get_append_location() requires that the ALBA is stored in offset_blocks.
	 * Additionally, offset_blocks has to be set before calling bdev_nvme_verify_pi_error().
	 */
	bdev_io->u.bdev.offset_blocks = *(uint64_t *)&cpl->cdw0;

	if (spdk_nvme_cpl_is_pi_error(cpl)) {
		SPDK_ERRLOG("zone append completed with PI error (sct=%d, sc=%d)\n",
			    cpl->status.sct, cpl->status.sc);
		/* Run PI verification for zone append data buffer if PI error is detected. */
		bdev_nvme_verify_pi_error(bio);
	}

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static void
bdev_nvme_comparev_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	if (spdk_nvme_cpl_is_pi_error(cpl)) {
		SPDK_ERRLOG("comparev completed with PI error (sct=%d, sc=%d)\n",
			    cpl->status.sct, cpl->status.sc);
		/* Run PI verification for compare data buffer if PI error is detected. */
		bdev_nvme_verify_pi_error(bio);
	}

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static void
bdev_nvme_comparev_and_writev_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	/* Compare operation completion */
	if (!bio->first_fused_completed) {
		/* Save compare result for write callback */
		bio->cpl = *cpl;
		bio->first_fused_completed = true;
		return;
	}

	/* Write operation completion */
	if (spdk_nvme_cpl_is_error(&bio->cpl)) {
		/* If bio->cpl is already an error, it means the compare operation failed.  In that case,
		 * complete the IO with the compare operation's status.
		 */
		if (!spdk_nvme_cpl_is_error(cpl)) {
			SPDK_ERRLOG("Unexpected write success after compare failure.\n");
		}

		bdev_nvme_io_complete_nvme_status(bio, &bio->cpl);
	} else {
		bdev_nvme_io_complete_nvme_status(bio, cpl);
	}
}

static void
bdev_nvme_queued_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static int
fill_zone_from_report(struct spdk_bdev_zone_info *info, struct spdk_nvme_zns_zone_desc *desc)
{
	switch (desc->zt) {
	case SPDK_NVME_ZONE_TYPE_SEQWR:
		info->type = SPDK_BDEV_ZONE_TYPE_SEQWR;
		break;
	default:
		SPDK_ERRLOG("Invalid zone type: %#x in zone report\n", desc->zt);
		return -EIO;
	}

	switch (desc->zs) {
	case SPDK_NVME_ZONE_STATE_EMPTY:
		info->state = SPDK_BDEV_ZONE_STATE_EMPTY;
		break;
	case SPDK_NVME_ZONE_STATE_IOPEN:
		info->state = SPDK_BDEV_ZONE_STATE_IMP_OPEN;
		break;
	case SPDK_NVME_ZONE_STATE_EOPEN:
		info->state = SPDK_BDEV_ZONE_STATE_EXP_OPEN;
		break;
	case SPDK_NVME_ZONE_STATE_CLOSED:
		info->state = SPDK_BDEV_ZONE_STATE_CLOSED;
		break;
	case SPDK_NVME_ZONE_STATE_RONLY:
		info->state = SPDK_BDEV_ZONE_STATE_READ_ONLY;
		break;
	case SPDK_NVME_ZONE_STATE_FULL:
		info->state = SPDK_BDEV_ZONE_STATE_FULL;
		break;
	case SPDK_NVME_ZONE_STATE_OFFLINE:
		info->state = SPDK_BDEV_ZONE_STATE_OFFLINE;
		break;
	default:
		SPDK_ERRLOG("Invalid zone state: %#x in zone report\n", desc->zs);
		return -EIO;
	}

	info->zone_id = desc->zslba;
	info->write_pointer = desc->wp;
	info->capacity = desc->zcap;

	return 0;
}

static int
bdev_nvme_write_uncorrectable(struct nvme_bdev_io *bio, uint64_t lba_count, uint64_t lba)
{
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "write uncorrectable %" PRIu64 " blocks with offset %#" PRIx64 "\n",
		      lba_count, lba);

	rc = spdk_nvme_ns_cmd_write_uncorrectable(bio->io_path->nvme_ns->ns, bio->io_path->qpair->qpair,
			lba, lba_count, bdev_nvme_queued_done, bio);
	if (spdk_unlikely(rc != 0 && rc != -ENOMEM)) {
		SPDK_ERRLOG("write uncorrectable failed: rc = %d\n", rc);
	}

	return rc;
}

static void
bdev_nvme_get_zone_info_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	uint64_t zone_id = bdev_io->u.zone_mgmt.zone_id;
	uint32_t zones_to_copy = bdev_io->u.zone_mgmt.num_zones;
	struct spdk_bdev_zone_info *info = bdev_io->u.zone_mgmt.buf;
	uint64_t max_zones_per_buf, i;
	uint32_t zone_report_bufsize;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair;
	int ret;

	if (spdk_nvme_cpl_is_error(cpl)) {
		goto out_complete_io_nvme_cpl;
	}

	if (spdk_unlikely(!nvme_io_path_is_available(bio->io_path))) {
		ret = -ENXIO;
		goto out_complete_io_ret;
	}

	ns = bio->io_path->nvme_ns->ns;
	qpair = bio->io_path->qpair->qpair;

	zone_report_bufsize = spdk_nvme_ns_get_max_io_xfer_size(ns);
	max_zones_per_buf = (zone_report_bufsize - sizeof(*bio->zone_report_buf)) /
			    sizeof(bio->zone_report_buf->descs[0]);

	if (bio->zone_report_buf->nr_zones > max_zones_per_buf) {
		ret = -EINVAL;
		goto out_complete_io_ret;
	}

	if (!bio->zone_report_buf->nr_zones) {
		ret = -EINVAL;
		goto out_complete_io_ret;
	}

	for (i = 0; i < bio->zone_report_buf->nr_zones && bio->handled_zones < zones_to_copy; i++) {
		ret = fill_zone_from_report(&info[bio->handled_zones],
					    &bio->zone_report_buf->descs[i]);
		if (ret) {
			goto out_complete_io_ret;
		}
		bio->handled_zones++;
	}

	if (bio->handled_zones < zones_to_copy) {
		uint64_t zone_size_lba = spdk_nvme_zns_ns_get_zone_size_sectors(ns);
		uint64_t slba = zone_id + (zone_size_lba * bio->handled_zones);

		memset(bio->zone_report_buf, 0, zone_report_bufsize);
		ret = spdk_nvme_zns_report_zones(ns, qpair,
						 bio->zone_report_buf, zone_report_bufsize,
						 slba, SPDK_NVME_ZRA_LIST_ALL, true,
						 bdev_nvme_get_zone_info_done, bio);
		if (!ret) {
			return;
		} else {
			goto out_complete_io_ret;
		}
	}

out_complete_io_nvme_cpl:
	free(bio->zone_report_buf);
	bio->zone_report_buf = NULL;
	bdev_nvme_io_complete_nvme_status(bio, cpl);
	return;

out_complete_io_ret:
	free(bio->zone_report_buf);
	bio->zone_report_buf = NULL;
	bdev_nvme_io_complete(bio, ret);
}

static void
bdev_nvme_zone_management_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static void
bdev_nvme_admin_passthru_complete_nvme_status(void *ctx)
{
	struct nvme_bdev_io *bio = ctx;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	const struct spdk_nvme_cpl *cpl = &bio->cpl;

	assert(bdev_nvme_io_type_is_admin(bdev_io->type));

	__bdev_nvme_io_complete(bdev_io, 0, cpl);
}

static void
bdev_nvme_abort_complete(void *ctx)
{
	struct nvme_bdev_io *bio = ctx;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);

	if (spdk_nvme_cpl_is_abort_success(&bio->cpl)) {
		__bdev_nvme_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS, NULL);
	} else {
		__bdev_nvme_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED, NULL);
	}
}

static void
bdev_nvme_abort_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);

	bio->cpl = *cpl;
	spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io), bdev_nvme_abort_complete, bio);
}

static void
bdev_nvme_admin_passthru_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);

	bio->cpl = *cpl;
	spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io),
			     bdev_nvme_admin_passthru_complete_nvme_status, bio);
}

static void
bdev_nvme_queued_reset_sgl(void *ref, uint32_t sgl_offset)
{
	struct nvme_bdev_io *bio = ref;
	struct iovec *iov;

	bio->iov_offset = sgl_offset;
	for (bio->iovpos = 0; bio->iovpos < bio->iovcnt; bio->iovpos++) {
		iov = &bio->iovs[bio->iovpos];
		if (bio->iov_offset < iov->iov_len) {
			break;
		}

		bio->iov_offset -= iov->iov_len;
	}
}

static int
bdev_nvme_queued_next_sge(void *ref, void **address, uint32_t *length)
{
	struct nvme_bdev_io *bio = ref;
	struct iovec *iov;

	assert(bio->iovpos < bio->iovcnt);

	iov = &bio->iovs[bio->iovpos];

	*address = iov->iov_base;
	*length = iov->iov_len;

	if (bio->iov_offset) {
		assert(bio->iov_offset <= iov->iov_len);
		*address += bio->iov_offset;
		*length -= bio->iov_offset;
	}

	bio->iov_offset += *length;
	if (bio->iov_offset == iov->iov_len) {
		bio->iovpos++;
		bio->iov_offset = 0;
	}

	return 0;
}

static void
bdev_nvme_queued_reset_fused_sgl(void *ref, uint32_t sgl_offset)
{
	struct nvme_bdev_io *bio = ref;
	struct iovec *iov;

	bio->fused_iov_offset = sgl_offset;
	for (bio->fused_iovpos = 0; bio->fused_iovpos < bio->fused_iovcnt; bio->fused_iovpos++) {
		iov = &bio->fused_iovs[bio->fused_iovpos];
		if (bio->fused_iov_offset < iov->iov_len) {
			break;
		}

		bio->fused_iov_offset -= iov->iov_len;
	}
}

static int
bdev_nvme_queued_next_fused_sge(void *ref, void **address, uint32_t *length)
{
	struct nvme_bdev_io *bio = ref;
	struct iovec *iov;

	assert(bio->fused_iovpos < bio->fused_iovcnt);

	iov = &bio->fused_iovs[bio->fused_iovpos];

	*address = iov->iov_base;
	*length = iov->iov_len;

	if (bio->fused_iov_offset) {
		assert(bio->fused_iov_offset <= iov->iov_len);
		*address += bio->fused_iov_offset;
		*length -= bio->fused_iov_offset;
	}

	bio->fused_iov_offset += *length;
	if (bio->fused_iov_offset == iov->iov_len) {
		bio->fused_iovpos++;
		bio->fused_iov_offset = 0;
	}

	return 0;
}

static int
bdev_nvme_no_pi_readv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		      void *md, uint64_t lba_count, uint64_t lba)
{
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "read %" PRIu64 " blocks with offset %#" PRIx64 " without PI check\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	rc = spdk_nvme_ns_cmd_readv_with_md(bio->io_path->nvme_ns->ns,
					    bio->io_path->qpair->qpair,
					    lba, lba_count,
					    bdev_nvme_no_pi_readv_done, bio, 0,
					    bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
					    md, 0, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("no_pi_readv failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_readv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		void *md, uint64_t lba_count, uint64_t lba, uint32_t flags,
		struct spdk_memory_domain *domain, void *domain_ctx,
		struct spdk_accel_sequence *seq)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->qpair->qpair;
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "read %" PRIu64 " blocks with offset %#" PRIx64 "\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	if (domain != NULL || seq != NULL) {
		bio->ext_opts.size = SPDK_SIZEOF(&bio->ext_opts, accel_sequence);
		bio->ext_opts.memory_domain = domain;
		bio->ext_opts.memory_domain_ctx = domain_ctx;
		bio->ext_opts.io_flags = flags;
		bio->ext_opts.metadata = md;
		bio->ext_opts.accel_sequence = seq;

		if (iovcnt == 1) {
			rc = spdk_nvme_ns_cmd_read_ext(ns, qpair, iov[0].iov_base, lba, lba_count, bdev_nvme_readv_done,
						       bio, &bio->ext_opts);
		} else {
			rc = spdk_nvme_ns_cmd_readv_ext(ns, qpair, lba, lba_count,
							bdev_nvme_readv_done, bio,
							bdev_nvme_queued_reset_sgl,
							bdev_nvme_queued_next_sge,
							&bio->ext_opts);
		}
	} else if (iovcnt == 1) {
		rc = spdk_nvme_ns_cmd_read_with_md(ns, qpair, iov[0].iov_base,
						   md, lba, lba_count, bdev_nvme_readv_done,
						   bio, flags, 0, 0);
	} else {
		rc = spdk_nvme_ns_cmd_readv_with_md(ns, qpair, lba, lba_count,
						    bdev_nvme_readv_done, bio, flags,
						    bdev_nvme_queued_reset_sgl,
						    bdev_nvme_queued_next_sge, md, 0, 0);
	}

	if (spdk_unlikely(rc != 0 && rc != -ENOMEM)) {
		SPDK_ERRLOG("readv failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_writev(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		 void *md, uint64_t lba_count, uint64_t lba, uint32_t flags,
		 struct spdk_memory_domain *domain, void *domain_ctx,
		 struct spdk_accel_sequence *seq,
		 union spdk_bdev_nvme_cdw12 cdw12, union spdk_bdev_nvme_cdw13 cdw13)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->qpair->qpair;
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "write %" PRIu64 " blocks with offset %#" PRIx64 "\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	if (domain != NULL || seq != NULL) {
		bio->ext_opts.size = SPDK_SIZEOF(&bio->ext_opts, accel_sequence);
		bio->ext_opts.memory_domain = domain;
		bio->ext_opts.memory_domain_ctx = domain_ctx;
		bio->ext_opts.io_flags = flags | SPDK_NVME_IO_FLAGS_DIRECTIVE(cdw12.write.dtype);
		bio->ext_opts.cdw13 = cdw13.raw;
		bio->ext_opts.metadata = md;
		bio->ext_opts.accel_sequence = seq;

		if (iovcnt == 1) {
			rc = spdk_nvme_ns_cmd_write_ext(ns, qpair, iov[0].iov_base, lba, lba_count, bdev_nvme_writev_done,
							bio, &bio->ext_opts);
		} else {
			rc = spdk_nvme_ns_cmd_writev_ext(ns, qpair, lba, lba_count,
							 bdev_nvme_writev_done, bio,
							 bdev_nvme_queued_reset_sgl,
							 bdev_nvme_queued_next_sge,
							 &bio->ext_opts);
		}
	} else if (iovcnt == 1) {
		rc = spdk_nvme_ns_cmd_write_with_md(ns, qpair, iov[0].iov_base,
						    md, lba, lba_count, bdev_nvme_writev_done,
						    bio, flags, 0, 0);
	} else {
		rc = spdk_nvme_ns_cmd_writev_with_md(ns, qpair, lba, lba_count,
						     bdev_nvme_writev_done, bio, flags,
						     bdev_nvme_queued_reset_sgl,
						     bdev_nvme_queued_next_sge, md, 0, 0);
	}

	if (spdk_unlikely(rc != 0 && rc != -ENOMEM)) {
		SPDK_ERRLOG("writev failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_zone_appendv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		       void *md, uint64_t lba_count, uint64_t zslba,
		       uint32_t flags)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->qpair->qpair;
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "zone append %" PRIu64 " blocks to zone start lba %#" PRIx64 "\n",
		      lba_count, zslba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	if (iovcnt == 1) {
		rc = spdk_nvme_zns_zone_append_with_md(ns, qpair, iov[0].iov_base, md, zslba,
						       lba_count,
						       bdev_nvme_zone_appendv_done, bio,
						       flags,
						       0, 0);
	} else {
		rc = spdk_nvme_zns_zone_appendv_with_md(ns, qpair, zslba, lba_count,
							bdev_nvme_zone_appendv_done, bio, flags,
							bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
							md, 0, 0);
	}

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("zone append failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_comparev(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		   void *md, uint64_t lba_count, uint64_t lba,
		   uint32_t flags)
{
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "compare %" PRIu64 " blocks with offset %#" PRIx64 "\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	rc = spdk_nvme_ns_cmd_comparev_with_md(bio->io_path->nvme_ns->ns,
					       bio->io_path->qpair->qpair,
					       lba, lba_count,
					       bdev_nvme_comparev_done, bio, flags,
					       bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
					       md, 0, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("comparev failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_comparev_and_writev(struct nvme_bdev_io *bio, struct iovec *cmp_iov, int cmp_iovcnt,
			      struct iovec *write_iov, int write_iovcnt,
			      void *md, uint64_t lba_count, uint64_t lba, uint32_t flags)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->qpair->qpair;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "compare and write %" PRIu64 " blocks with offset %#" PRIx64 "\n",
		      lba_count, lba);

	bio->iovs = cmp_iov;
	bio->iovcnt = cmp_iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;
	bio->fused_iovs = write_iov;
	bio->fused_iovcnt = write_iovcnt;
	bio->fused_iovpos = 0;
	bio->fused_iov_offset = 0;

	if (bdev_io->num_retries == 0) {
		bio->first_fused_submitted = false;
		bio->first_fused_completed = false;
	}

	if (!bio->first_fused_submitted) {
		flags |= SPDK_NVME_IO_FLAGS_FUSE_FIRST;
		memset(&bio->cpl, 0, sizeof(bio->cpl));

		rc = spdk_nvme_ns_cmd_comparev_with_md(ns, qpair, lba, lba_count,
						       bdev_nvme_comparev_and_writev_done, bio, flags,
						       bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge, md, 0, 0);
		if (rc == 0) {
			bio->first_fused_submitted = true;
			flags &= ~SPDK_NVME_IO_FLAGS_FUSE_FIRST;
		} else {
			if (rc != -ENOMEM) {
				SPDK_ERRLOG("compare failed: rc = %d\n", rc);
			}
			return rc;
		}
	}

	flags |= SPDK_NVME_IO_FLAGS_FUSE_SECOND;

	rc = spdk_nvme_ns_cmd_writev_with_md(ns, qpair, lba, lba_count,
					     bdev_nvme_comparev_and_writev_done, bio, flags,
					     bdev_nvme_queued_reset_fused_sgl, bdev_nvme_queued_next_fused_sge, md, 0, 0);
	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("write failed: rc = %d\n", rc);
		rc = 0;
	}

	return rc;
}

static int
bdev_nvme_unmap(struct nvme_bdev_io *bio, uint64_t offset_blocks, uint64_t num_blocks)
{
	struct spdk_nvme_dsm_range dsm_ranges[SPDK_NVME_DATASET_MANAGEMENT_MAX_RANGES];
	struct spdk_nvme_dsm_range *range;
	uint64_t offset, remaining;
	uint64_t num_ranges_u64;
	uint16_t num_ranges;
	int rc;

	num_ranges_u64 = (num_blocks + SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS - 1) /
			 SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS;
	if (num_ranges_u64 > SPDK_COUNTOF(dsm_ranges)) {
		SPDK_ERRLOG("Unmap request for %" PRIu64 " blocks is too large\n", num_blocks);
		return -EINVAL;
	}
	num_ranges = (uint16_t)num_ranges_u64;

	offset = offset_blocks;
	remaining = num_blocks;
	range = &dsm_ranges[0];

	/* Fill max-size ranges until the remaining blocks fit into one range */
	while (remaining > SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS) {
		range->attributes.raw = 0;
		range->length = SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS;
		range->starting_lba = offset;

		offset += SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS;
		remaining -= SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS;
		range++;
	}

	/* Final range describes the remaining blocks */
	range->attributes.raw = 0;
	range->length = remaining;
	range->starting_lba = offset;

	rc = spdk_nvme_ns_cmd_dataset_management(bio->io_path->nvme_ns->ns,
			bio->io_path->qpair->qpair,
			SPDK_NVME_DSM_ATTR_DEALLOCATE,
			dsm_ranges, num_ranges,
			bdev_nvme_queued_done, bio);

	return rc;
}

static int
bdev_nvme_write_zeroes(struct nvme_bdev_io *bio, uint64_t offset_blocks, uint64_t num_blocks)
{
	if (num_blocks > UINT16_MAX + 1) {
		SPDK_ERRLOG("NVMe write zeroes is limited to 16-bit block count\n");
		return -EINVAL;
	}

	return spdk_nvme_ns_cmd_write_zeroes(bio->io_path->nvme_ns->ns,
					     bio->io_path->qpair->qpair,
					     offset_blocks, num_blocks,
					     bdev_nvme_queued_done, bio,
					     0);
}

static int
bdev_nvme_flush(struct nvme_bdev_io *bio)
{
	return spdk_nvme_ns_cmd_flush(bio->io_path->nvme_ns->ns,
				      bio->io_path->qpair->qpair,
				      bdev_nvme_queued_done, bio);
}

static int
bdev_nvme_get_zone_info(struct nvme_bdev_io *bio, uint64_t zone_id, uint32_t num_zones,
			struct spdk_bdev_zone_info *info)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->qpair->qpair;
	uint32_t zone_report_bufsize = spdk_nvme_ns_get_max_io_xfer_size(ns);
	uint64_t zone_size = spdk_nvme_zns_ns_get_zone_size_sectors(ns);
	uint64_t total_zones = spdk_nvme_zns_ns_get_num_zones(ns);

	if (zone_id % zone_size != 0) {
		return -EINVAL;
	}

	if (num_zones > total_zones || !num_zones) {
		return -EINVAL;
	}

	assert(!bio->zone_report_buf);
	bio->zone_report_buf = calloc(1, zone_report_bufsize);
	if (!bio->zone_report_buf) {
		return -ENOMEM;
	}

	bio->handled_zones = 0;

	return spdk_nvme_zns_report_zones(ns, qpair, bio->zone_report_buf, zone_report_bufsize,
					  zone_id, SPDK_NVME_ZRA_LIST_ALL, true,
					  bdev_nvme_get_zone_info_done, bio);
}

static int
bdev_nvme_zone_management(struct nvme_bdev_io *bio, uint64_t zone_id,
			  enum spdk_bdev_zone_action action)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->qpair->qpair;

	switch (action) {
	case SPDK_BDEV_ZONE_CLOSE:
		return spdk_nvme_zns_close_zone(ns, qpair, zone_id, false,
						bdev_nvme_zone_management_done, bio);
	case SPDK_BDEV_ZONE_FINISH:
		return spdk_nvme_zns_finish_zone(ns, qpair, zone_id, false,
						 bdev_nvme_zone_management_done, bio);
	case SPDK_BDEV_ZONE_OPEN:
		return spdk_nvme_zns_open_zone(ns, qpair, zone_id, false,
					       bdev_nvme_zone_management_done, bio);
	case SPDK_BDEV_ZONE_RESET:
		return spdk_nvme_zns_reset_zone(ns, qpair, zone_id, false,
						bdev_nvme_zone_management_done, bio);
	case SPDK_BDEV_ZONE_OFFLINE:
		return spdk_nvme_zns_offline_zone(ns, qpair, zone_id, false,
						  bdev_nvme_zone_management_done, bio);
	default:
		return -EINVAL;
	}
}

static void
bdev_nvme_admin_passthru(struct nvme_bdev_channel *nbdev_ch, struct nvme_bdev_io *bio,
			 struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes)
{
	struct nvme_io_path *io_path;
	struct nvme_ctrlr *nvme_ctrlr;
	uint32_t max_xfer_size;
	int rc = -ENXIO;

	/* Choose the first ctrlr which is not failed. */
	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		nvme_ctrlr = io_path->qpair->ctrlr;

		/* We should skip any unavailable nvme_ctrlr rather than checking
		 * if the return value of spdk_nvme_ctrlr_cmd_admin_raw() is -ENXIO.
		 */
		if (!nvme_ctrlr_is_available(nvme_ctrlr)) {
			continue;
		}

		max_xfer_size = spdk_nvme_ctrlr_get_max_xfer_size(nvme_ctrlr->ctrlr);

		if (nbytes > max_xfer_size) {
			NVME_CTRLR_ERRLOG(nvme_ctrlr, "nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
			rc = -EINVAL;
			goto err;
		}

		rc = spdk_nvme_ctrlr_cmd_admin_raw(nvme_ctrlr->ctrlr, cmd, buf, (uint32_t)nbytes,
						   bdev_nvme_admin_passthru_done, bio);
		if (rc == 0) {
			return;
		}
	}

err:
	bdev_nvme_admin_complete(bio, rc);
}

static int
bdev_nvme_io_passthru(struct nvme_bdev_io *bio, struct spdk_nvme_cmd *cmd,
		      void *buf, size_t nbytes)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->qpair->qpair;
	uint32_t max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);

	if (nbytes > max_xfer_size) {
		NVME_QPAIR_ERRLOG(bio->io_path->qpair, "nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
		return -EINVAL;
	}

	/*
	 * Each NVMe bdev is a specific namespace, and all NVMe I/O commands require a nsid,
	 * so fill it out automatically.
	 */
	cmd->nsid = spdk_nvme_ns_get_id(ns);

	return spdk_nvme_ctrlr_cmd_io_raw(ctrlr, qpair, cmd, buf,
					  (uint32_t)nbytes, bdev_nvme_queued_done, bio);
}

static int
bdev_nvme_io_passthru_md(struct nvme_bdev_io *bio, struct spdk_nvme_cmd *cmd,
			 void *buf, size_t nbytes, void *md_buf, size_t md_len)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->qpair->qpair;
	size_t nr_sectors = nbytes / spdk_nvme_ns_get_extended_sector_size(ns);
	uint32_t max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);

	if (nbytes > max_xfer_size) {
		NVME_QPAIR_ERRLOG(bio->io_path->qpair, "nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
		return -EINVAL;
	}

	if (md_len != nr_sectors * spdk_nvme_ns_get_md_size(ns)) {
		NVME_QPAIR_ERRLOG(bio->io_path->qpair, "invalid meta data buffer size\n");
		return -EINVAL;
	}

	/*
	 * Each NVMe bdev is a specific namespace, and all NVMe I/O commands require a nsid,
	 * so fill it out automatically.
	 */
	cmd->nsid = spdk_nvme_ns_get_id(ns);

	return spdk_nvme_ctrlr_cmd_io_raw_with_md(ctrlr, qpair, cmd, buf,
			(uint32_t)nbytes, md_buf, bdev_nvme_queued_done, bio);
}

static int
bdev_nvme_iov_passthru_md(struct nvme_bdev_io *bio,
			  struct spdk_nvme_cmd *cmd, struct iovec *iov, int iovcnt,
			  size_t nbytes, void *md_buf, size_t md_len)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->qpair->qpair;
	size_t nr_sectors = nbytes / spdk_nvme_ns_get_extended_sector_size(ns);
	uint32_t max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	if (nbytes > max_xfer_size) {
		NVME_QPAIR_ERRLOG(bio->io_path->qpair, "nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
		return -EINVAL;
	}

	if (md_len != nr_sectors * spdk_nvme_ns_get_md_size(ns)) {
		NVME_QPAIR_ERRLOG(bio->io_path->qpair, "invalid meta data buffer size\n");
		return -EINVAL;
	}

	/*
	 * Each NVMe bdev is a specific namespace, and all NVMe I/O commands
	 * require a nsid, so fill it out automatically.
	 */
	cmd->nsid = spdk_nvme_ns_get_id(ns);

	return spdk_nvme_ctrlr_cmd_iov_raw_with_md(
		       ctrlr, qpair, cmd, (uint32_t)nbytes, md_buf, bdev_nvme_queued_done, bio,
		       bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge);
}

static void
bdev_nvme_abort(struct nvme_bdev_channel *nbdev_ch, struct nvme_bdev_io *bio,
		struct nvme_bdev_io *bio_to_abort)
{
	struct nvme_io_path *io_path;
	int rc = 0;

	rc = bdev_nvme_abort_retry_io(nbdev_ch, bio_to_abort);
	if (rc == 0) {
		bdev_nvme_admin_complete(bio, 0);
		return;
	}

	io_path = bio_to_abort->io_path;
	if (io_path != NULL) {
		rc = spdk_nvme_ctrlr_cmd_abort_ext(io_path->qpair->ctrlr->ctrlr,
						   io_path->qpair->qpair,
						   bio_to_abort,
						   bdev_nvme_abort_done, bio);
	} else {
		STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
			rc = spdk_nvme_ctrlr_cmd_abort_ext(io_path->qpair->ctrlr->ctrlr,
							   NULL,
							   bio_to_abort,
							   bdev_nvme_abort_done, bio);

			if (rc != -ENOENT) {
				break;
			}
		}
	}

	if (rc != 0) {
		/* If no command was found or there was any error, complete the abort
		 * request with failure.
		 */
		bdev_nvme_admin_complete(bio, rc);
	}
}

static int
bdev_nvme_copy(struct nvme_bdev_io *bio, uint64_t dst_offset_blocks, uint64_t src_offset_blocks,
	       uint64_t num_blocks)
{
	struct spdk_nvme_scc_source_range range = {
		.slba = src_offset_blocks,
		.nlb = num_blocks - 1
	};

	return spdk_nvme_ns_cmd_copy(bio->io_path->nvme_ns->ns,
				     bio->io_path->qpair->qpair,
				     &range, 1, dst_offset_blocks,
				     bdev_nvme_queued_done, bio);
}

static void
bdev_nvme_opts_config_json(struct spdk_json_write_ctx *w)
{
	const char *action;
	uint32_t i;

	if (g_opts.action_on_timeout == SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET) {
		action = "reset";
	} else if (g_opts.action_on_timeout == SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT) {
		action = "abort";
	} else {
		action = "none";
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_nvme_set_options");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "action_on_timeout", action);
	spdk_json_write_named_uint64(w, "timeout_us", g_opts.timeout_us);
	spdk_json_write_named_uint64(w, "timeout_admin_us", g_opts.timeout_admin_us);
	spdk_json_write_named_uint32(w, "keep_alive_timeout_ms", g_opts.keep_alive_timeout_ms);
	spdk_json_write_named_uint32(w, "arbitration_burst", g_opts.arbitration_burst);
	spdk_json_write_named_uint32(w, "low_priority_weight", g_opts.low_priority_weight);
	spdk_json_write_named_uint32(w, "medium_priority_weight", g_opts.medium_priority_weight);
	spdk_json_write_named_uint32(w, "high_priority_weight", g_opts.high_priority_weight);
	spdk_json_write_named_uint64(w, "nvme_adminq_poll_period_us", g_opts.nvme_adminq_poll_period_us);
	spdk_json_write_named_uint64(w, "nvme_ioq_poll_period_us", g_opts.nvme_ioq_poll_period_us);
	spdk_json_write_named_uint32(w, "io_queue_requests", g_opts.io_queue_requests);
	spdk_json_write_named_bool(w, "delay_cmd_submit", g_opts.delay_cmd_submit);
	spdk_json_write_named_uint32(w, "transport_retry_count", g_opts.transport_retry_count);
	spdk_json_write_named_int32(w, "bdev_retry_count", g_opts.bdev_retry_count);
	spdk_json_write_named_uint8(w, "transport_ack_timeout", g_opts.transport_ack_timeout);
	spdk_json_write_named_int32(w, "ctrlr_loss_timeout_sec", g_opts.ctrlr_loss_timeout_sec);
	spdk_json_write_named_uint32(w, "reconnect_delay_sec", g_opts.reconnect_delay_sec);
	spdk_json_write_named_uint32(w, "fast_io_fail_timeout_sec", g_opts.fast_io_fail_timeout_sec);
	spdk_json_write_named_bool(w, "disable_auto_failback", g_opts.disable_auto_failback);
	spdk_json_write_named_bool(w, "generate_uuids", g_opts.generate_uuids);
	spdk_json_write_named_uint8(w, "transport_tos", g_opts.transport_tos);
	spdk_json_write_named_bool(w, "nvme_error_stat", g_opts.nvme_error_stat);
	spdk_json_write_named_uint32(w, "rdma_srq_size", g_opts.rdma_srq_size);
	spdk_json_write_named_bool(w, "io_path_stat", g_opts.io_path_stat);
	spdk_json_write_named_bool(w, "allow_accel_sequence", g_opts.allow_accel_sequence);
	spdk_json_write_named_uint32(w, "rdma_max_cq_size", g_opts.rdma_max_cq_size);
	spdk_json_write_named_uint16(w, "rdma_cm_event_timeout_ms", g_opts.rdma_cm_event_timeout_ms);
	spdk_json_write_named_array_begin(w, "dhchap_digests");
	for (i = 0; i < 32; ++i) {
		if (g_opts.dhchap_digests & SPDK_BIT(i)) {
			spdk_json_write_string(w, spdk_nvme_dhchap_get_digest_name(i));
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_named_array_begin(w, "dhchap_dhgroups");
	for (i = 0; i < 32; ++i) {
		if (g_opts.dhchap_dhgroups & SPDK_BIT(i)) {
			spdk_json_write_string(w, spdk_nvme_dhchap_get_dhgroup_name(i));
		}
	}

	spdk_json_write_array_end(w);
	spdk_json_write_named_bool(w, "rdma_umr_per_io", g_opts.rdma_umr_per_io);
	spdk_json_write_named_uint32(w, "tcp_connect_timeout_ms", g_opts.tcp_connect_timeout_ms);
	spdk_json_write_named_bool(w, "enable_flush", g_opts.enable_flush);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
bdev_nvme_discovery_config_json(struct spdk_json_write_ctx *w, struct discovery_ctx *ctx)
{
	struct spdk_nvme_transport_id trid;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_nvme_start_discovery");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", ctx->name);
	spdk_json_write_named_string(w, "hostnqn", ctx->hostnqn);

	trid = ctx->trid;
	memset(trid.subnqn, 0, sizeof(trid.subnqn));
	nvme_bdev_dump_trid_json(&trid, w);

	spdk_json_write_named_bool(w, "wait_for_attach", ctx->wait_for_attach);
	spdk_json_write_named_int32(w, "ctrlr_loss_timeout_sec", ctx->bdev_opts.ctrlr_loss_timeout_sec);
	spdk_json_write_named_uint32(w, "reconnect_delay_sec", ctx->bdev_opts.reconnect_delay_sec);
	spdk_json_write_named_uint32(w, "fast_io_fail_timeout_sec",
				     ctx->bdev_opts.fast_io_fail_timeout_sec);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

#ifdef SPDK_CONFIG_NVME_CUSE
static void
nvme_ctrlr_cuse_config_json(struct spdk_json_write_ctx *w,
			    struct nvme_ctrlr *nvme_ctrlr)
{
	size_t cuse_name_size = 128;
	char cuse_name[cuse_name_size];

	if (spdk_nvme_cuse_get_ctrlr_name(nvme_ctrlr->ctrlr,
					  cuse_name, &cuse_name_size) != 0) {
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_nvme_cuse_register");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", nvme_ctrlr->nbdev_ctrlr->name);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}
#endif

static void
nvme_ctrlr_config_json(struct spdk_json_write_ctx *w,
		       struct nvme_ctrlr *nvme_ctrlr,
		       struct spdk_nvme_path_id *path_id)
{
	struct spdk_nvme_transport_id	*trid;
	const struct spdk_nvme_ctrlr_opts *opts;

	if (nvme_ctrlr->opts.from_discovery_service) {
		/* Do not emit an RPC for this - it will be implicitly
		 * covered by a separate bdev_nvme_start_discovery or
		 * bdev_nvme_start_mdns_discovery RPC.
		 */
		return;
	}

	trid = &path_id->trid;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_nvme_attach_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", nvme_ctrlr->nbdev_ctrlr->name);
	nvme_bdev_dump_trid_json(trid, w);
	spdk_json_write_named_bool(w, "prchk_reftag",
				   (nvme_ctrlr->opts.prchk_flags & SPDK_NVME_IO_FLAGS_PRCHK_REFTAG) != 0);
	spdk_json_write_named_bool(w, "prchk_guard",
				   (nvme_ctrlr->opts.prchk_flags & SPDK_NVME_IO_FLAGS_PRCHK_GUARD) != 0);
	spdk_json_write_named_int32(w, "ctrlr_loss_timeout_sec", nvme_ctrlr->opts.ctrlr_loss_timeout_sec);
	spdk_json_write_named_uint32(w, "reconnect_delay_sec", nvme_ctrlr->opts.reconnect_delay_sec);
	spdk_json_write_named_uint32(w, "fast_io_fail_timeout_sec",
				     nvme_ctrlr->opts.fast_io_fail_timeout_sec);
	if (nvme_ctrlr->psk != NULL) {
		spdk_json_write_named_string(w, "psk", spdk_key_get_name(nvme_ctrlr->psk));
	}
	if (nvme_ctrlr->dhchap_key != NULL) {
		spdk_json_write_named_string(w, "dhchap_key",
					     spdk_key_get_name(nvme_ctrlr->dhchap_key));
	}
	if (nvme_ctrlr->dhchap_ctrlr_key != NULL) {
		spdk_json_write_named_string(w, "dhchap_ctrlr_key",
					     spdk_key_get_name(nvme_ctrlr->dhchap_ctrlr_key));
	}
	opts = spdk_nvme_ctrlr_get_opts(nvme_ctrlr->ctrlr);
	spdk_json_write_named_string(w, "hostnqn", opts->hostnqn);
	spdk_json_write_named_bool(w, "hdgst", opts->header_digest);
	spdk_json_write_named_bool(w, "ddgst", opts->data_digest);
	if (opts->src_addr[0] != '\0') {
		spdk_json_write_named_string(w, "hostaddr", opts->src_addr);
	}
	if (opts->src_svcid[0] != '\0') {
		spdk_json_write_named_string(w, "hostsvcid", opts->src_svcid);
	}

	if (nvme_ctrlr->opts.multipath) {
		spdk_json_write_named_string(w, "multipath", "multipath");
	}
	spdk_json_write_named_uint64(w, "fabrics_connect_timeout_us", opts->fabrics_connect_timeout_us);
	spdk_json_write_named_uint32(w, "num_io_queues", opts->num_io_queues);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
bdev_nvme_hotplug_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "bdev_nvme_set_hotplug");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint64(w, "period_us", g_nvme_hotplug_poll_period_us);
	spdk_json_write_named_bool(w, "enable", g_nvme_hotplug_enabled);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
bdev_nvme_multipath_config_json(struct nvme_bdev *nbdev, struct spdk_json_write_ctx *w)
{
	/* Skip dump if it is matching the default conf. */
	if (nbdev->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE &&
	    nbdev->mp_selector == BDEV_NVME_MP_SELECTOR_ROUND_ROBIN && nbdev->rr_min_io == UINT32_MAX) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "bdev_nvme_set_multipath_policy");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", nbdev->disk.name);
	spdk_json_write_named_string(w, "policy", nvme_bdev_get_mp_policy_str(nbdev));
	if (nbdev->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE) {
		spdk_json_write_named_string(w, "selector", nvme_bdev_get_mp_selector_str(nbdev));
		if (nbdev->mp_selector == BDEV_NVME_MP_SELECTOR_ROUND_ROBIN) {
			spdk_json_write_named_uint32(w, "rr_min_io", nbdev->rr_min_io);
		}
	}

	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static int
bdev_nvme_config_json(struct spdk_json_write_ctx *w)
{
	struct nvme_bdev_ctrlr	*nbdev_ctrlr;
	struct nvme_ctrlr	*nvme_ctrlr;
	struct discovery_ctx	*ctx;
	struct spdk_nvme_path_id	*path_id;

	assert(spdk_thread_is_app_thread(NULL));

	bdev_nvme_opts_config_json(w);

	TAILQ_FOREACH(nbdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		struct nvme_bdev *nbdev;

		TAILQ_FOREACH(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq) {
			path_id = nvme_ctrlr->active_path_id;
			assert(path_id == TAILQ_FIRST(&nvme_ctrlr->trids));
			nvme_ctrlr_config_json(w, nvme_ctrlr, path_id);

			path_id = TAILQ_NEXT(path_id, link);
			while (path_id != NULL) {
				nvme_ctrlr_config_json(w, nvme_ctrlr, path_id);
				path_id = TAILQ_NEXT(path_id, link);
			}

#ifdef SPDK_CONFIG_NVME_CUSE
			nvme_ctrlr_cuse_config_json(w, nvme_ctrlr);
#endif
		}

		TAILQ_FOREACH(nbdev, &nbdev_ctrlr->bdevs, tailq) {
			bdev_nvme_multipath_config_json(nbdev, w);
		}
	}

	TAILQ_FOREACH(ctx, &g_discovery_ctxs, tailq) {
		if (!ctx->from_mdns_discovery_service) {
			bdev_nvme_discovery_config_json(w, ctx);
		}
	}

	bdev_nvme_mdns_discovery_config_json(w);

	/* Dump as last parameter to give all NVMe bdevs chance to be constructed
	 * before enabling hotplug poller.
	 */
	bdev_nvme_hotplug_config_json(w);
	return 0;
}

/*
 * [한국어]
 * bdev_nvme_get_ctrlr - bdev_nvme.h §2 참조. spdk_bdev * → spdk_nvme_ctrlr *.
 *
 * 외부 모듈(예: vbdev_opal)이 NVMe Identify 같은 정보를 직접 조회하기 위해 사용.
 * bdev이 nvme bdev이 아니면 NULL. 멀티패스의 경우 첫 path의 컨트롤러를 반환.
 */
struct spdk_nvme_ctrlr *
bdev_nvme_get_ctrlr(struct spdk_bdev *bdev)
{
	struct nvme_bdev *nbdev;
	struct nvme_ns *nvme_ns;

	assert(spdk_thread_is_app_thread(NULL));

	if (!bdev || bdev->module != &nvme_if) {
		return NULL;
	}

	nbdev = SPDK_CONTAINEROF(bdev, struct nvme_bdev, disk);
	nvme_ns = TAILQ_FIRST(&nbdev->nvme_ns_list);
	assert(nvme_ns != NULL);

	return nvme_ns->ctrlr->ctrlr;
}

static bool
nvme_io_path_is_current(struct nvme_io_path *io_path)
{
	const struct nvme_bdev_channel *nbdev_ch;
	bool current;

	if (!nvme_io_path_is_available(io_path)) {
		return false;
	}

	nbdev_ch = io_path->nbdev_ch;
	if (nbdev_ch == NULL) {
		current = false;
	} else if (nbdev_ch->mp_policy == BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE) {
		struct nvme_io_path *optimized_io_path = NULL;

		STAILQ_FOREACH(optimized_io_path, &nbdev_ch->io_path_list, stailq) {
			if (optimized_io_path->nvme_ns->ana_state == SPDK_NVME_ANA_OPTIMIZED_STATE) {
				break;
			}
		}

		/* A non-optimized path is only current if there are no optimized paths. */
		current = (io_path->nvme_ns->ana_state == SPDK_NVME_ANA_OPTIMIZED_STATE) ||
			  (optimized_io_path == NULL);
	} else {
		current = (io_path == nbdev_ch->current_io_path);
	}

	return current;
}

static struct nvme_ctrlr *
bdev_nvme_next_ctrlr(struct nvme_bdev_ctrlr *nbdev_ctrlr, struct nvme_ctrlr *prev)
{
	struct nvme_ctrlr *next = NULL;

	assert((!!nbdev_ctrlr) != (!!prev));
	assert(spdk_thread_is_app_thread(NULL));

	if (prev) {
		next = TAILQ_NEXT(prev, tailq);
	} else if (nbdev_ctrlr) {
		next = TAILQ_FIRST(&nbdev_ctrlr->ctrlrs);
	}
	while (next != NULL) {
		/* ref can be 0 when the ctrlr was released, but hasn't been detached yet */
		pthread_mutex_lock(&next->mutex);
		if (next->ref > 0) {
			next->ref++;
			pthread_mutex_unlock(&next->mutex);
			return next;
		}

		pthread_mutex_unlock(&next->mutex);
		next = TAILQ_NEXT(next, tailq);
	}

	return NULL;
}

struct bdev_nvme_set_keys_ctx {
	struct nvme_ctrlr	*nctrlr;
	struct spdk_key		*dhchap_key;
	struct spdk_key		*dhchap_ctrlr_key;
	bdev_nvme_set_keys_cb	cb_fn;
	void			*cb_ctx;
};

static void
bdev_nvme_free_set_keys_ctx(struct bdev_nvme_set_keys_ctx *ctx)
{
	if (ctx == NULL) {
		return;
	}

	spdk_keyring_put_key(ctx->dhchap_key);
	spdk_keyring_put_key(ctx->dhchap_ctrlr_key);
	free(ctx);
}

static void
bdev_nvme_set_keys_done(struct bdev_nvme_set_keys_ctx *ctx, int status)
{
	ctx->cb_fn(ctx->cb_ctx, status);
	if (ctx->nctrlr != NULL) {
		nvme_ctrlr_put_ref(ctx->nctrlr);
	}

	bdev_nvme_free_set_keys_ctx(ctx);
}

static void bdev_nvme_authenticate_ctrlr(struct bdev_nvme_set_keys_ctx *ctx);

static void
bdev_nvme_authenticate_ctrlr_continue(struct bdev_nvme_set_keys_ctx *ctx)
{
	struct nvme_ctrlr *next;

	assert(spdk_thread_is_app_thread(NULL));

	next = bdev_nvme_next_ctrlr(NULL, ctx->nctrlr);
	nvme_ctrlr_put_ref(ctx->nctrlr);
	ctx->nctrlr = next;

	if (next == NULL) {
		bdev_nvme_set_keys_done(ctx, 0);
	} else {
		bdev_nvme_authenticate_ctrlr(ctx);
	}
}

static void
bdev_nvme_authenticate_qpairs_done(struct spdk_io_channel_iter *i, int status)
{
	struct bdev_nvme_set_keys_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	if (status != 0) {
		bdev_nvme_set_keys_done(ctx, status);
		return;
	}
	bdev_nvme_authenticate_ctrlr_continue(ctx);
}

static void
bdev_nvme_authenticate_qpair_done(void *ctx, int status)
{
	spdk_for_each_channel_continue(ctx, status);
}

static void
bdev_nvme_authenticate_qpair(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_ctrlr_channel *ctrlr_ch = spdk_io_channel_get_ctx(ch);
	struct nvme_qpair *qpair = ctrlr_ch->qpair;
	int rc;

	if (!nvme_qpair_is_connected(qpair)) {
		spdk_for_each_channel_continue(i, 0);
		return;
	}

	rc = spdk_nvme_qpair_authenticate(qpair->qpair, bdev_nvme_authenticate_qpair_done, i);
	if (rc != 0) {
		spdk_for_each_channel_continue(i, rc);
	}
}

static void
bdev_nvme_authenticate_ctrlr_done(void *_ctx, int status)
{
	struct bdev_nvme_set_keys_ctx *ctx = _ctx;

	if (status != 0) {
		bdev_nvme_set_keys_done(ctx, status);
		return;
	}

	spdk_for_each_channel(ctx->nctrlr, bdev_nvme_authenticate_qpair, ctx,
			      bdev_nvme_authenticate_qpairs_done);
}

static void
bdev_nvme_authenticate_ctrlr(struct bdev_nvme_set_keys_ctx *ctx)
{
	struct spdk_nvme_ctrlr_key_opts opts = {};
	struct nvme_ctrlr *nctrlr = ctx->nctrlr;
	int rc;

	opts.size = SPDK_SIZEOF(&opts, dhchap_ctrlr_key);
	opts.dhchap_key = ctx->dhchap_key;
	opts.dhchap_ctrlr_key = ctx->dhchap_ctrlr_key;
	rc = spdk_nvme_ctrlr_set_keys(nctrlr->ctrlr, &opts);
	if (rc != 0) {
		bdev_nvme_set_keys_done(ctx, rc);
		return;
	}

	if (ctx->dhchap_key != NULL) {
		rc = spdk_nvme_ctrlr_authenticate(nctrlr->ctrlr,
						  bdev_nvme_authenticate_ctrlr_done, ctx);
		if (rc != 0) {
			bdev_nvme_set_keys_done(ctx, rc);
		}
	} else {
		bdev_nvme_authenticate_ctrlr_continue(ctx);
	}
}

/*
 * [한국어]
 * bdev_nvme_set_keys - bdev_nvme.h §2 참조. 그룹 내 모든 컨트롤러의 DH-CHAP 키 갱신.
 *
 * 동작:
 *   1) ctx 할당, 키 매니저(spdk_keyring)에서 dhchap_key/dhchap_ctrlr_key를 lookup해 ref 보유.
 *   2) 그룹의 첫 컨트롤러부터 bdev_nvme_authenticate_ctrlr 시작.
 *      각 컨트롤러에 대해 lib/nvme의 DH-CHAP 재인증 시퀀스 실행.
 *   3) 모든 컨트롤러 인증 완료 후 cb_fn 호출.
 * 비동기 함수: 반환값은 시작 결과만, 최종 결과는 cb로 전달.
 */
int
bdev_nvme_set_keys(const char *name, const char *dhchap_key, const char *dhchap_ctrlr_key,
		   bdev_nvme_set_keys_cb cb_fn, void *cb_ctx)
{
	struct bdev_nvme_set_keys_ctx *ctx;
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nctrlr;

	assert(spdk_thread_is_app_thread(NULL));

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	if (dhchap_key != NULL) {
		ctx->dhchap_key = spdk_keyring_get_key(dhchap_key);
		if (ctx->dhchap_key == NULL) {
			SPDK_ERRLOG("Could not find key %s for bdev %s\n", dhchap_key, name);
			bdev_nvme_free_set_keys_ctx(ctx);
			return -ENOKEY;
		}
	}
	if (dhchap_ctrlr_key != NULL) {
		ctx->dhchap_ctrlr_key = spdk_keyring_get_key(dhchap_ctrlr_key);
		if (ctx->dhchap_ctrlr_key == NULL) {
			SPDK_ERRLOG("Could not find key %s for bdev %s\n", dhchap_ctrlr_key, name);
			bdev_nvme_free_set_keys_ctx(ctx);
			return -ENOKEY;
		}
	}

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (nbdev_ctrlr == NULL) {
		SPDK_ERRLOG("Could not find bdev_ctrlr %s\n", name);
		bdev_nvme_free_set_keys_ctx(ctx);
		return -ENODEV;
	}
	nctrlr = bdev_nvme_next_ctrlr(nbdev_ctrlr, NULL);
	if (nctrlr == NULL) {
		SPDK_ERRLOG("Could not find any nvme_ctrlrs on bdev_ctrlr %s\n", name);
		bdev_nvme_free_set_keys_ctx(ctx);
		return -ENODEV;
	}

	ctx->nctrlr = nctrlr;
	ctx->cb_fn = cb_fn;
	ctx->cb_ctx = cb_ctx;

	bdev_nvme_authenticate_ctrlr(ctx);

	return 0;
}

/*
 * [한국어]
 * nvme_io_path_info_json - bdev_nvme.h §2 참조. io_path의 정보를 JSON으로 직렬화.
 *
 * 출력 필드: bdev_name, cntlid, current(현재 활성 path 여부), connected, accessible,
 *           ANA state, trid, hostnqn, stat (있을 때), 등.
 * RPC bdev_nvme_get_io_paths의 응답 빌드에서 호출.
 */
void
nvme_io_path_info_json(struct spdk_json_write_ctx *w, struct nvme_io_path *io_path)
{
	struct nvme_ns *nvme_ns = io_path->nvme_ns;
	struct nvme_ctrlr *nvme_ctrlr = io_path->qpair->ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;
	const struct spdk_nvme_transport_id *trid;
	const char *adrfam_str;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "bdev_name", nvme_ns->bdev->disk.name);

	cdata = spdk_nvme_ctrlr_get_data(nvme_ctrlr->ctrlr);
	trid = spdk_nvme_ctrlr_get_transport_id(nvme_ctrlr->ctrlr);

	spdk_json_write_named_uint32(w, "cntlid", cdata->cntlid);
	spdk_json_write_named_bool(w, "current", nvme_io_path_is_current(io_path));
	spdk_json_write_named_bool(w, "connected", nvme_qpair_is_connected(io_path->qpair));
	spdk_json_write_named_bool(w, "accessible", nvme_ns_is_accessible(nvme_ns));

	spdk_json_write_named_object_begin(w, "transport");
	spdk_json_write_named_string(w, "trtype", trid->trstring);
	spdk_json_write_named_string(w, "traddr", trid->traddr);
	if (trid->trsvcid[0] != '\0') {
		spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);
	}
	adrfam_str = spdk_nvme_transport_id_adrfam_str(trid->adrfam);
	if (adrfam_str) {
		spdk_json_write_named_string(w, "adrfam", adrfam_str);
	}
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/*
 * [한국어]
 * bdev_nvme_get_discovery_info - bdev_nvme.h §2 참조. 디스커버리 세션 정보 JSON 출력.
 *
 * 출력 형식: 배열의 각 원소는 {name, trid, referrals: [{trid: ...}]}.
 * referrals는 디스커버리 컨트롤러를 통해 발견된 NVM 서브시스템 trid들.
 */
void
bdev_nvme_get_discovery_info(struct spdk_json_write_ctx *w)
{
	struct discovery_ctx *ctx;
	struct discovery_entry_ctx *entry_ctx;

	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(ctx, &g_discovery_ctxs, tailq) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "name", ctx->name);

		spdk_json_write_named_object_begin(w, "trid");
		nvme_bdev_dump_trid_json(&ctx->trid, w);
		spdk_json_write_object_end(w);

		spdk_json_write_named_array_begin(w, "referrals");
		TAILQ_FOREACH(entry_ctx, &ctx->discovery_entry_ctxs, tailq) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_object_begin(w, "trid");
			nvme_bdev_dump_trid_json(&entry_ctx->trid, w);
			spdk_json_write_object_end(w);
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_nvme)

static void
bdev_nvme_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"BDEV_NVME_IO_START", TRACE_BDEV_NVME_IO_START,
			OWNER_TYPE_NONE, OBJECT_BDEV_NVME_IO, 1,
			{{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 }}
		},
		{
			"BDEV_NVME_IO_DONE", TRACE_BDEV_NVME_IO_DONE,
			OWNER_TYPE_NONE, OBJECT_BDEV_NVME_IO, 0,
			{{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 }}
		}
	};


	spdk_trace_register_object(OBJECT_BDEV_NVME_IO, 'N');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
	spdk_trace_tpoint_register_relation(TRACE_NVME_PCIE_SUBMIT, OBJECT_BDEV_NVME_IO, 0);
	spdk_trace_tpoint_register_relation(TRACE_NVME_TCP_SUBMIT, OBJECT_BDEV_NVME_IO, 0);
	spdk_trace_tpoint_register_relation(TRACE_NVME_PCIE_COMPLETE, OBJECT_BDEV_NVME_IO, 0);
	spdk_trace_tpoint_register_relation(TRACE_NVME_TCP_COMPLETE, OBJECT_BDEV_NVME_IO, 0);
}
SPDK_TRACE_REGISTER_FN(bdev_nvme_trace, "bdev_nvme", TRACE_GROUP_BDEV_NVME)
