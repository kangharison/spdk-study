/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe transport abstraction
 */

/*
 * [한국어 설명] NVMe 트랜스포트 추상화 디스패치 레이어 (nvme_transport.c)
 *
 * === 파일의 역할 ===
 * SPDK NVMe 드라이버가 지원하는 모든 트랜스포트(PCIe/RDMA/TCP/VFIO-USER/CUSE)를
 * **공통 vtable 인터페이스**로 묶는 디스패치 레이어. 이 파일의 거의 모든 함수는
 * "이 컨트롤러(또는 qpair)의 트랜스포트를 찾아서, 해당 트랜스포트의 ops 함수를 호출한다"는
 * 단일 패턴을 따른다.
 *
 * 상위 레이어(nvme_qpair.c, nvme_ctrlr.c, nvme_ns_cmd.c 등)는 트랜스포트 종류를 모른 채
 * 이 파일의 `nvme_transport_*` 래퍼만 호출한다. 각 트랜스포트 구현(nvme_pcie.c, nvme_rdma.c,
 * nvme_tcp.c 등)은 `SPDK_NVME_TRANSPORT_REGISTER()` 매크로로 자신의 `spdk_nvme_transport_ops`
 * 구조체를 이 파일의 전역 TAILQ(`g_spdk_nvme_transports`)에 삽입한다.
 *
 * === vtable 디스패치 패턴 ===
 * 모든 래퍼는 사실상 한 가지 동작을 반복한다:
 *
 *     const struct spdk_nvme_transport *t = nvme_get_transport(ctrlr->trid.trstring);
 *     assert(t != NULL);
 *     return t->ops.xxx(ctrlr, ...);
 *
 * 차이점은 세 가지 변주:
 *   (A) **Optional op**: 일부 op는 모든 트랜스포트가 구현하지 않음 (예: CMB/PMR은 PCIe만).
 *       → 호출 전 `if (t->ops.xxx != NULL)`로 가드하고 미지원이면 -ENOTSUP/NULL 반환.
 *   (B) **Async fallback**: 비동기 레지스터 접근은 트랜스포트가 미구현이면 동기 호출 후
 *       가짜 완료를 `ctrlr->register_operations` 큐에 넣어 admin qpair의
 *       `spdk_nvme_qpair_process_completions`가 나중에 콜백 실행 (nvme_qpair.c의
 *       nvme_complete_register_operations와 짝).
 *   (C) **Hot-path 캐싱**: I/O qpair는 자기 `qpair->transport` 필드에 포인터를 캐시해두고
 *       hot path에서는 이 캐시만 쓴다 (linear scan 생략). Admin qpair만 예외적으로
 *       매번 `nvme_get_transport(trstring)`로 조회.
 *
 * === qpair->transport 캐시 vs nvme_get_transport(trstring) 조회: 왜 둘 다? ===
 * 주요 이유는 **NVMe PCIe의 multi-process 지원**이다.
 *   - SPDK는 primary/secondary 프로세스가 같은 NVMe SSD를 공유할 수 있다 (DPDK 기반).
 *   - `spdk_nvme_transport` 객체는 함수 포인터를 담고 있는데, fork 없이 secondary가 attach할 때
 *     secondary 프로세스의 주소 공간에서는 primary가 가리키던 함수 포인터 값이 무효할 수 있다.
 *     (실제로는 각 프로세스마다 자신의 g_transports[] 정적 배열에 포인터를 새로 등록.)
 *   - 따라서 **admin 큐**(multi-process 공유)는 `qpair->transport` 캐시를 안 쓰고
 *     매번 `nvme_get_transport(ctrlr->trid.trstring)`로 자기 프로세스의 ops를 찾아 호출.
 *   - **I/O 큐**는 한 프로세스가 독점하므로 `qpair->transport` 캐시 사용 (hot-path 빠름).
 *
 * 이 패턴이 `nvme_transport_qpair_abort_reqs / reset / submit_request /
 * process_completions / iterate_requests`에 공통적으로 나타난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (제출):
 *   [nvme_qpair.c] _nvme_qpair_submit_request
 *     → nvme_transport_qpair_submit_request(qpair, req)   [이 파일, line 668]
 *         → qpair->transport->ops.qpair_submit_request    [I/O qpair → 캐시 경로]
 *         → transport->ops.qpair_submit_request           [admin qpair → 조회 경로]
 *             → nvme_pcie_qpair_submit_request()          [nvme_pcie_common.c]
 *                 → SQ[sq_tail] = SQE; doorbell MMIO
 *
 * 호출 체인 (완료):
 *   [nvme_qpair.c] spdk_nvme_qpair_process_completions
 *     → nvme_transport_qpair_process_completions(qpair, max)   [이 파일, line 682]
 *         → qpair->transport->ops.qpair_process_completions    [I/O]
 *         → transport->ops.qpair_process_completions           [admin]
 *             → nvme_pcie_qpair_process_completions()          [nvme_pcie_common.c]
 *                 → CQ[cq_head].phase 검사 → nvme_complete_request
 *
 * === 타 모듈과의 연결 ===
 *   - `nvme_internal.h`           — struct spdk_nvme_transport_ops 정의 (80+ 함수 포인터),
 *                                    spdk_nvme_transport/poll_group 구조체, 상위·하위 API 프로토타입.
 *   - `nvme_pcie.c / pcie_common.c` → `SPDK_NVME_TRANSPORT_REGISTER()`로 "PCIE" ops 등록.
 *   - `nvme_rdma.c / tcp.c / vfio_user.c / cuse.c` → 각각 고유 이름으로 ops 등록.
 *   - `nvme_ctrlr.c`              — 컨트롤러 생성·스캔·레지스터 접근에서 이 파일의 래퍼 호출.
 *   - `nvme_qpair.c`              — submit/complete/abort/reset에서 이 파일의 래퍼 호출.
 *   - `nvme_poll_group.c`         — 상위 poll group 추상이 이 파일의 transport_poll_group_* 호출.
 *   - `spdk_nvme_transport_opts` 전역 구조체 — RDMA SRQ 크기, TCP connect timeout 등 전역 파라미터.
 *
 * === 주요 함수/구조체 요약 ===
 *   ★ nvme_transport_qpair_submit_request / process_completions  — 핫 패스 I/O 디스패치
 *   ★ nvme_transport_ctrlr_connect_qpair / disconnect_qpair      — qpair 상태머신을 transport로 연결
 *   ★ nvme_transport_ctrlr_set/get_reg_{4,8}_async              — async 미구현 트랜스포트용 sync fallback + 완료 큐잉
 *     nvme_transport_poll_group_create / add / remove / process   — 트랜스포트별 poll group 수명주기
 *     spdk_nvme_transport_register                               — 트랜스포트가 시작 시 자기 ops 등록
 *     nvme_get_transport / first / next                          — 이름/순회로 트랜스포트 찾기
 *     spdk_nvme_transport_get_opts / set_opts                    — 전역 RDMA/TCP 파라미터 ABI 호환 접근
 *
 * 관련 상수 / 전역:
 *   SPDK_MAX_NUM_OF_TRANSPORTS(16)  — 정적 등록 슬롯 수
 *   g_transports[]                   — 실제 객체 스토리지 (static array, 프로세스별)
 *   g_spdk_nvme_transports (TAILQ)   — 등록 순회 리스트 (링크 포인터만 관리)
 *   g_current_transport_index        — g_transports[] 다음 빈 슬롯 인덱스
 *   g_spdk_nvme_transport_opts       — 전역 RDMA/TCP 설정 (set_opts로 수정, 등록된 트랜스포트들이 공유)
 *
 * 이 파일은 "분기 단계의 얇은 레이어"처럼 보이지만, **multi-process 안전성·async fallback·
 * hot/cold 경계**를 모두 담고 있어 NVMe 드라이버 아키텍처의 중심축이다.
 */

#include "nvme_internal.h"       /* [한국어] 내부 타입 전체 — spdk_nvme_ctrlr/qpair, transport_ops,
                                  *         probe_ctx, register_completion 등 이 파일이 조작하는 대상 */
#include "spdk/queue.h"          /* [한국어] TAILQ_* / STAILQ_* 매크로 — 트랜스포트 리스트, poll_group qpair 큐 사용 */

#define SPDK_MAX_NUM_OF_TRANSPORTS 16
                                  /* [한국어] 정적 등록 슬롯 수. 실제로는 PCIe/RDMA/TCP/VFIOUSER/CUSE 정도이므로
                                   *         16은 넉넉. g_transports[] 배열 크기이자 등록 상한. */

struct spdk_nvme_transport {
                                  /* [한국어] 등록된 트랜스포트의 내부 표현.
                                   *         - ops: 해당 트랜스포트의 함수 포인터 vtable (공용 인터페이스)
                                   *         - link: g_spdk_nvme_transports TAILQ에 포함되기 위한 연결자 */
	struct spdk_nvme_transport_ops	ops;
                                  /* [한국어] 트랜스포트 ops — ctrlr_construct, qpair_submit_request 등 80+ 함수 포인터.
                                   *         nvme_internal.h에 선언됨. */
	TAILQ_ENTRY(spdk_nvme_transport)	link;
                                  /* [한국어] 이중 연결 리스트 포인터 (TAILQ) — 등록 순회용. */
};

TAILQ_HEAD(nvme_transport_list, spdk_nvme_transport) g_spdk_nvme_transports =
	TAILQ_HEAD_INITIALIZER(g_spdk_nvme_transports);
                                  /* [한국어] 전역 트랜스포트 리스트. 각 트랜스포트 구현체가 자기 초기화 시
                                   *         SPDK_NVME_TRANSPORT_REGISTER 매크로를 통해 이 리스트에 추가됨.
                                   *         프로세스별로 별도 존재 (multi-process 시 각 프로세스가 자기 리스트를 가짐). */

static struct spdk_nvme_transport g_transports[SPDK_MAX_NUM_OF_TRANSPORTS] = {};
                                  /* [한국어] 실제 객체 스토리지 (정적 배열). TAILQ는 이 배열의 엔트리를 링크만 건다.
                                   *         왜 static array인가: 동적 할당 회피 — 트랜스포트 등록은 생애 극초반이며
                                   *         실패 시 assert로 fail-fast, 평생 free 없음. */
static int g_current_transport_index = 0;
                                  /* [한국어] 다음 배정 가능한 g_transports 슬롯 인덱스 (0~15). 등록할 때마다 증가. */

struct spdk_nvme_transport_opts g_spdk_nvme_transport_opts = {
                                  /* [한국어] 전역 트랜스포트 설정. RDMA/TCP 트랜스포트 초기화 시 참조.
                                   *         spdk_nvme_transport_set_opts()로 런타임 변경 가능.
                                   *         값별 의미:
                                   *           - rdma_srq_size=0: SRQ(Shared Receive Queue) 비활성 (각 QP 개별 RQ)
                                   *           - rdma_max_cq_size=0: CQ 크기 자동 결정
                                   *           - rdma_cm_event_timeout_ms=1000: RDMA CM 이벤트 대기 시간
                                   *           - rdma_umr_per_io=false: UMR(User-Mode Reg)를 I/O마다 쓰지 않음
                                   *           - tcp_connect_timeout_ms=0: TCP connect 타임아웃 무한 (기본) */
	.rdma_srq_size = 0,
	.rdma_max_cq_size = 0,
	.rdma_cm_event_timeout_ms = 1000,
	.rdma_umr_per_io = false,
	.tcp_connect_timeout_ms = 0,
};

/*
 * [한국어] nvme_get_first_transport - 등록된 트랜스포트 리스트의 첫 번째 엔트리 반환.
 *
 * 호출자: probe 경로의 `nvme_transport_ctrlr_scan`이 "auto" 지정 시 모든 트랜스포트를 순회하며 스캔.
 * @return: TAILQ 선두 엔트리, 없으면 NULL.
 */
const struct spdk_nvme_transport *
nvme_get_first_transport(void)
{
	return TAILQ_FIRST(&g_spdk_nvme_transports);
                                  /* [한국어] TAILQ의 선두 — 첫 번째 등록된 트랜스포트 */
}

/*
 * [한국어] nvme_get_next_transport - 순회용 다음 엔트리 반환.
 *
 * 호출자: auto-probe 루프가 first→next→next ... NULL을 만날 때까지 순회.
 */
const struct spdk_nvme_transport *
nvme_get_next_transport(const struct spdk_nvme_transport *transport)
{
	return TAILQ_NEXT(transport, link);
                                  /* [한국어] 주어진 엔트리의 다음 엔트리 (없으면 NULL) */
}

/*
 * Unfortunately, due to NVMe PCIe multiprocess support, we cannot store the
 * transport object in either the controller struct or the admin qpair. This means
 * that a lot of admin related transport calls will have to call nvme_get_transport
 * in order to know which functions to call.
 * In the I/O path, we have the ability to store the transport struct in the I/O
 * qpairs to avoid taking a performance hit.
 */
/*
 * [한국어] nvme_get_transport - 이름으로 등록된 트랜스포트 객체 조회.
 *
 * @transport_name: "PCIE", "RDMA", "TCP", "VFIOUSER", "CUSE" 등 트랜스포트 식별 문자열
 *                  (대소문자 무시 비교)
 * @return: 매칭되는 spdk_nvme_transport 포인터, 미등록이면 NULL
 *
 * 위 영문 주석이 중요한 배경 설명: PCIe multi-process 지원 때문에 트랜스포트 객체 포인터를
 * ctrlr/qpair에 영속적으로 저장할 수 없다. 각 프로세스가 자신의 g_transports[] 배열에
 * 서로 다른 주소로 객체를 할당하기 때문. 따라서 admin 경로에서는 매 호출마다 이 함수로
 * **지금 이 프로세스의** 트랜스포트 객체를 찾아야 한다. I/O qpair는 프로세스 전용이므로
 * qpair->transport에 캐시해도 안전하다.
 *
 * 호출자: 거의 모든 `nvme_transport_*` 래퍼 (admin 경로), qpair 생성 시 `qpair->transport` 초기 세팅 등.
 */
const struct spdk_nvme_transport *
nvme_get_transport(const char *transport_name)
{
	struct spdk_nvme_transport *registered_transport;
                                  /* [한국어] 순회 반복자 */

	TAILQ_FOREACH(registered_transport, &g_spdk_nvme_transports, link) {
                                  /* [한국어] 전역 리스트를 선형 검색 (최대 16개이므로 비용 무시 가능) */
		if (strcasecmp(transport_name, registered_transport->ops.name) == 0) {
                                  /* [한국어] 대소문자 무시 비교 — 사용자 입력이 "pcie"/"PCIe" 등 다양할 수 있음 */
			return registered_transport;
		}
	}

	return NULL;
                                  /* [한국어] 미등록 — 호출자는 보통 -ENOENT/-ENOTSUP 반환 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_transport_available — trtype enum으로 트랜스포트 등록 여부 질의.
 *
 * @trtype: SPDK_NVME_TRANSPORT_PCIE/RDMA/TCP/... enum
 * @return: true=해당 트랜스포트 라이브러리가 링크되어 등록됨
 *
 * 예: spdk_nvme_transport_available(SPDK_NVME_TRANSPORT_RDMA)가 false면 librdmacm을 링크 안 했거나
 *     UCX/MLX 라이브러리 미존재로 RDMA 트랜스포트 미등록.
 */
bool
spdk_nvme_transport_available(enum spdk_nvme_transport_type trtype)
{
	return nvme_get_transport(spdk_nvme_transport_id_trtype_str(trtype)) == NULL ? false : true;
                                  /* [한국어] enum → 문자열 변환 후 조회. NULL이면 false, 아니면 true */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_transport_available_by_name — 문자열로 등록 여부 질의.
 *
 * 위 함수의 문자열 버전 — 커스텀 트랜스포트 이름까지 커버.
 */
bool
spdk_nvme_transport_available_by_name(const char *transport_name)
{
	return nvme_get_transport(transport_name) == NULL ? false : true;
                                  /* [한국어] 이름 직접 사용 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_transport_register — 트랜스포트 등록 (매크로에서 사용) ★
 *
 * 각 트랜스포트 구현(nvme_pcie.c 등)은 파일 맨 아래에서 `SPDK_NVME_TRANSPORT_REGISTER(NAME, ops)`
 * 매크로를 쓴다. 이 매크로는 `__attribute__((constructor))` 함수를 만들어 main() 진입 전에
 * 이 함수를 호출 → 자동으로 g_spdk_nvme_transports에 ops 등록.
 *
 * 이중 등록 탐지 + g_transports[] 슬롯 초과 탐지.
 * 실패 시 assert — 등록은 프로세스 시작 중 딱 한 번만 발생해야 하는 크리티컬 경로.
 *
 * 실행 컨텍스트: 프로세스 시작 중 단일 스레드 (libc ctor 단계)이므로 락 불필요.
 */
void
spdk_nvme_transport_register(const struct spdk_nvme_transport_ops *ops)
{
	struct spdk_nvme_transport *new_transport;
                                  /* [한국어] 새로 배정할 g_transports[] 슬롯 포인터 */

	if (nvme_get_transport(ops->name)) {
                                  /* [한국어] 같은 이름이 이미 등록되어 있으면 프로그래머 실수 — assert로 실패 */
		SPDK_ERRLOG("Double registering NVMe transport %s is prohibited.\n", ops->name);
		assert(false);
	}

	if (g_current_transport_index == SPDK_MAX_NUM_OF_TRANSPORTS) {
                                  /* [한국어] 슬롯 한계 도달 — 정적 배열이므로 확장 불가 */
		SPDK_ERRLOG("Unable to register new NVMe transport.\n");
		assert(false);
		return;
	}
	new_transport = &g_transports[g_current_transport_index++];
                                  /* [한국어] 현재 빈 슬롯을 차지하고 인덱스 +1 — 이후 영원히 이 슬롯은 이 트랜스포트 소유 */

	new_transport->ops = *ops;
                                  /* [한국어] 구조체 복사 — 호출자의 ops는 로컬/스택일 수 있으므로 값 복사로 안전 확보 */
	TAILQ_INSERT_TAIL(&g_spdk_nvme_transports, new_transport, link);
                                  /* [한국어] 순회 리스트 꼬리에 연결 — 등록 순서 보존 */
}

/*
 * [한국어] nvme_transport_ctrlr_construct - 컨트롤러 객체 생성 (트랜스포트별 attach 진입점).
 *
 * @trid:     트랜스포트 식별자 (trstring="PCIE", traddr="0000:81:00.0" 등)
 * @opts:     사용자 요청 옵션 (io_queue_size, keep_alive_timeout 등)
 * @devhandle: PCIe의 경우 DPDK `rte_pci_device *`, 다른 트랜스포트는 NULL 가능
 * @return:   할당·초기화된 struct spdk_nvme_ctrlr*, 실패 시 NULL
 *
 * 호출자: `nvme_probe_internal` → probe_ctx의 각 trid에 대해 호출.
 * 실제 구현: nvme_pcie_ctrlr_construct, nvme_rdma_ctrlr_construct 등.
 *
 * ★ 여기서부터 이 파일의 "컨트롤러 수명주기" 래퍼 시퀀스가 시작된다:
 *   construct → scan → enable → enable_interrupts(옵션) → ready → 사용 → destruct
 */
struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(trid->trstring);
                                  /* [한국어] trstring으로 트랜스포트 조회 */
	struct spdk_nvme_ctrlr *ctrlr;
                                  /* [한국어] 반환할 컨트롤러 객체 */

	if (transport == NULL) {
                                  /* [한국어] 알 수 없는 트랜스포트 이름 — 프로그래머 실수(probe trid 입력 검증 누락) */
		SPDK_ERRLOG("Transport %s doesn't exist.", trid->trstring);
		return NULL;
	}

	ctrlr = transport->ops.ctrlr_construct(trid, opts, devhandle);
                                  /* [한국어] 트랜스포트별 실제 construct 호출 — 할당 + trid 복사 + opts 저장 + 기본 초기화 */

	return ctrlr;
                                  /* [한국어] 성공 시 주 소자, 실패 시 NULL (콜러가 구별) */
}

/*
 * [한국어] nvme_transport_ctrlr_scan - 특정 트랜스포트에서 NVMe 장치 탐색.
 *
 * @direct_connect: true면 `trid`로 정확히 지정된 한 장치만 연결 (bdev_nvme의 attach 경로),
 *                   false면 해당 트랜스포트의 모든 장치를 나열 (spdk_nvme_probe).
 *
 * 트랜스포트별 구현:
 *   - PCIe: DPDK가 enumerate한 PCI 장치 중 NVMe 클래스 장치 필터링
 *   - RDMA/TCP: Discovery Controller에 연결해 log page로 subsystem 목록 조회
 *
 * probe_ctx는 스캔 결과를 축적 (probe_cb 콜백을 통해 사용자에게 통지).
 */
int
nvme_transport_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
			  bool direct_connect)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(probe_ctx->trid.trstring);

	if (transport == NULL) {
                                  /* [한국어] trid가 지정한 트랜스포트 미등록 */
		SPDK_ERRLOG("Transport %s doesn't exist.", probe_ctx->trid.trstring);
		return -ENOENT;
	}

	return transport->ops.ctrlr_scan(probe_ctx, direct_connect);
                                  /* [한국어] 트랜스포트별 스캔 함수 위임 */
}

/*
 * [한국어] nvme_transport_ctrlr_scan_attached - 이미 attach된 컨트롤러만 재스캔 (namespace 변경 감지).
 *
 * 용도: hotplug로 namespace가 추가/제거될 때, 재스캔으로 기존 컨트롤러의 NS 목록 갱신.
 * 모든 트랜스포트가 구현하지는 않음 — 미구현이면 -ENOTSUP.
 */
int
nvme_transport_ctrlr_scan_attached(struct spdk_nvme_probe_ctx *probe_ctx)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(probe_ctx->trid.trstring);

	if (transport == NULL) {
		SPDK_ERRLOG("Transport %s doesn't exist.", probe_ctx->trid.trstring);
		return -ENOENT;
	}

	if (transport->ops.ctrlr_scan_attached != NULL) {
                                  /* [한국어] optional op — 구현되어 있을 때만 호출 */
		return transport->ops.ctrlr_scan_attached(probe_ctx);
	}
	SPDK_ERRLOG("Transport %s does not support ctrlr_scan_attached callback\n",
		    probe_ctx->trid.trstring);
	return -ENOTSUP;
                                  /* [한국어] 미구현 트랜스포트는 "기능 없음" 알림 */
}

/*
 * [한국어] nvme_transport_ctrlr_destruct - 컨트롤러 해제 (construct의 짝).
 *
 * 호출자: `nvme_ctrlr_destruct_async_poll` 수명주기 종료 단계.
 * 실제 구현이 qpair들 정리, 레지스터 복귀, 메모리 해제 등 처리.
 */
int
nvme_transport_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
                                  /* [한국어] ctrlr이 살아있는 이상 transport도 반드시 등록되어 있어야 함 */
	return transport->ops.ctrlr_destruct(ctrlr);
                                  /* [한국어] 트랜스포트별 해제 로직 (mandatory op — 모든 트랜스포트 필수) */
}

/*
 * [한국어] nvme_transport_ctrlr_enable - CC.EN=1로 컨트롤러 활성화.
 *
 * NVMe Base Spec의 CC(Controller Configuration).EN 비트 전환. 활성화 시
 * 컨트롤러가 admin 큐의 SQE를 fetch하기 시작 → CSTS.RDY=1이 되길 기다림.
 *
 * 트랜스포트별 구현 이유: PCIe는 MMIO BAR write, Fabrics는 property_set capsule.
 * 호출자: `nvme_ctrlr_process_init`의 CHECK_EN → ENABLE 전이 단계.
 */
int
nvme_transport_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_enable(ctrlr);
                                  /* [한국어] 트랜스포트별 CC.EN=1 수행 (mandatory op) */
}

/*
 * [한국어] nvme_transport_ctrlr_enable_interrupts - polled → interrupt 모드 전환 훅.
 *
 * SPDK는 기본 polled-mode지만, 전력 절약 등을 위해 일부 트랜스포트가 interrupt 모드를
 * 지원하면 이 훅에서 MSI-X 벡터 바인딩, epoll fd 노출 등을 수행.
 * PCIe의 경우 `nvme_pcie_ctrlr_enable_interrupts`가 VFIO MSI-X 핸들러를 연결.
 *
 * Optional op — TCP/RDMA는 기본적으로 socket/fd epoll이므로 별도 훅 불필요 (-ENOTSUP).
 */
int
nvme_transport_ctrlr_enable_interrupts(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.ctrlr_enable_interrupts != NULL) {
		return transport->ops.ctrlr_enable_interrupts(ctrlr);
	}

	return -ENOTSUP;
                                  /* [한국어] 미구현 트랜스포트는 interrupt 모드 전환 불가 */
}

/*
 * [한국어] nvme_transport_ctrlr_ready - init 상태머신 마지막 단계 진입 훅 (optional).
 *
 * 호출자: `nvme_ctrlr_process_init`이 READY 상태로 전이할 때.
 * 트랜스포트가 init 완료 직전 후처리를 하고 싶을 때 구현 (예: 초기 AER 제출, stats 초기화 등).
 * 미구현이면 0 반환 (successful no-op).
 */
int
nvme_transport_ctrlr_ready(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.ctrlr_ready) {
                                  /* [한국어] optional op 존재 확인 */
		return transport->ops.ctrlr_ready(ctrlr);
	}

	return 0;
                                  /* [한국어] 미구현은 성공 처리 (아무것도 할 일 없음) */
}

/*
 * [한국어] ★ 레지스터 동기 R/W 4종 - NVMe 스펙 레지스터를 32/64비트 폭으로 직접 접근 ★
 *
 * NVMe 스펙 Section 3.1의 컨트롤러 레지스터들 (CAP, VS, CC, CSTS, AQA, ASQ, ACQ, CMBLOC, ...).
 * PCIe는 MMIO BAR0 매핑을 통해 직접 접근하고, Fabrics는 property get/set capsule을 주고받음.
 *
 * 4종 시그니처:
 *   set_reg_4(offset, value)       — 32비트 write (CC, CSTS 등)
 *   set_reg_8(offset, value)       — 64비트 write (ASQ, ACQ 64비트 주소)
 *   get_reg_4(offset, *value)      — 32비트 read
 *   get_reg_8(offset, *value)      — 64비트 read (CAP는 64비트)
 *
 * 호출자: `nvme_ctrlr_process_init`의 상태머신이 CC/CSTS 전이 시 대량으로 사용.
 *
 * 모든 트랜스포트가 4종 필수 구현 (mandatory).
 */
int
nvme_transport_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_set_reg_4(ctrlr, offset, value);
                                  /* [한국어] 32비트 레지스터 쓰기 — PCIe면 MMIO, fabric이면 property_set capsule */
}

int
nvme_transport_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_set_reg_8(ctrlr, offset, value);
                                  /* [한국어] 64비트 쓰기 — ASQ/ACQ 물리 주소 세팅에 사용 */
}

int
nvme_transport_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_get_reg_4(ctrlr, offset, value);
                                  /* [한국어] 32비트 읽기 (value 포인터에 결과 저장) */
}

int
nvme_transport_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_get_reg_8(ctrlr, offset, value);
                                  /* [한국어] 64비트 읽기 — CAP(Controller Capabilities)가 대표 대상 */
}

/*
 * [한국어] ★ nvme_queue_register_operation_completion - async 레지스터 R/W의 가짜 완료 큐잉 헬퍼 ★
 *
 * 트랜스포트가 async 레지스터 op를 **구현하지 않은** 경우 fallback으로 쓰이는 호출 패턴:
 *   1) 동기 op로 즉시 접근
 *   2) 가짜 완료 ctx를 `ctrlr->register_operations` STAILQ에 삽입
 *   3) 나중에 admin qpair의 `spdk_nvme_qpair_process_completions` →
 *      `nvme_complete_register_operations` (nvme_qpair.c)가 이 ctx를 소비하여 cb_fn 호출
 *
 * 이 패턴의 의의: 상위 코드가 "비동기"인 것처럼 작성되어 있어도, 트랜스포트가 구현 안 했을 때
 * "즉시 동기 호출 + 뒤늦은 콜백"으로 같은 API 동작을 흉내낼 수 있다.
 *
 * @value: 읽기 op의 결과 또는 쓰기 op의 입력값 — cb_fn이 검증에 사용.
 * @cb_fn, @cb_ctx: 사용자 콜백과 그 컨텍스트.
 * @return: 0=성공 큐잉, -ENOMEM=ctx 할당 실패
 *
 * 동기화: ctrlr_lock(multi-process safe spinlock)으로 STAILQ 보호.
 * 메모리: SPDK_MALLOC_SHARE — primary/secondary 모두 접근 가능하도록 hugepage 할당.
 */
static int
nvme_queue_register_operation_completion(struct spdk_nvme_ctrlr *ctrlr, uint64_t value,
		spdk_nvme_reg_cb cb_fn, void *cb_ctx)
{
	struct nvme_register_completion *ctx;
                                  /* [한국어] 큐에 삽입할 가짜 완료 컨텍스트 */

	ctx = spdk_zmalloc(sizeof(*ctx), 0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
                                  /* [한국어] hugepage에 할당 — multi-process 공유 가능, NUMA 제약 없음 */
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	ctx->cpl.status.sc = SPDK_NVME_SC_SUCCESS;
                                  /* [한국어] cpl.status = SUCCESS — sync op가 이미 성공했으므로 */
	ctx->cb_fn = cb_fn;
	ctx->cb_ctx = cb_ctx;
	ctx->value = value;
                                  /* [한국어] 읽기 op의 반환값 또는 쓰기 op의 value */
	ctx->pid = getpid();
                                  /* [한국어] multi-process에서 이 ctx는 내 프로세스가 실행해야 함 표시 */

	nvme_ctrlr_lock(ctrlr);
                                  /* [한국어] STAILQ 수정 — 다른 프로세스와 경쟁 가능 */
	STAILQ_INSERT_TAIL(&ctrlr->register_operations, ctx, stailq);
                                  /* [한국어] 완료 큐 꼬리에 추가 — admin process_completions가 pid 필터로 소비 */
	nvme_ctrlr_unlock(ctrlr);

	return 0;
}

/*
 * [한국어] ★ 비동기 레지스터 R/W 4종 - async 미구현 트랜스포트를 위한 자동 fallback ★
 *
 * 각 함수 패턴:
 *   if (ops.xxx_async == NULL) {
 *       // async 미구현 → 동기 호출 + 가짜 완료 큐잉
 *       sync_op();
 *       queue_register_operation_completion(...);
 *   } else {
 *       // async 구현됨 → 그대로 위임
 *       ops.xxx_async(...);
 *   }
 *
 * 왜 필요한가: admin 큐 기반 비동기 경로는 fabric 트랜스포트에서 자연스럽지만, PCIe는 MMIO가 본질적으로 동기.
 * 상위 코드가 "async" 단일 API만 부르면 되도록 이 파일에서 동기/비동기 간극을 숨긴다.
 *
 * 호출자: ctrlr reset의 비동기 상태머신 (`nvme_ctrlr_process_init`의 async variant)에서
 *         CC/CSTS/AQA 등 레지스터를 연쇄적으로 접근.
 */
int
nvme_transport_ctrlr_set_reg_4_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value,
				     spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);
	int rc;

	assert(transport != NULL);
	if (transport->ops.ctrlr_set_reg_4_async == NULL) {
                                  /* [한국어] async 미구현 → sync fallback */
		rc = transport->ops.ctrlr_set_reg_4(ctrlr, offset, value);
                                  /* [한국어] 동기 쓰기 */
		if (rc != 0) {
			return rc;
                                  /* [한국어] sync 실패 — 호출자가 즉시 처리, 큐잉 불필요 */
		}

		return nvme_queue_register_operation_completion(ctrlr, value, cb_fn, cb_arg);
                                  /* [한국어] 가짜 완료 큐잉 — 다음 admin process_completions에서 cb_fn 실행 */
	}

	return transport->ops.ctrlr_set_reg_4_async(ctrlr, offset, value, cb_fn, cb_arg);
                                  /* [한국어] async 구현 존재 → 그대로 호출 */
}

int
nvme_transport_ctrlr_set_reg_8_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value,
				     spdk_nvme_reg_cb cb_fn, void *cb_arg)

{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);
	int rc;

	assert(transport != NULL);
	if (transport->ops.ctrlr_set_reg_8_async == NULL) {
                                  /* [한국어] 64비트 쓰기 async 미구현 — sync fallback */
		rc = transport->ops.ctrlr_set_reg_8(ctrlr, offset, value);
		if (rc != 0) {
			return rc;
		}

		return nvme_queue_register_operation_completion(ctrlr, value, cb_fn, cb_arg);
	}

	return transport->ops.ctrlr_set_reg_8_async(ctrlr, offset, value, cb_fn, cb_arg);
}

int
nvme_transport_ctrlr_get_reg_4_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				     spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);
	uint32_t value;
                                  /* [한국어] sync fallback에서 읽어올 값의 임시 변수 */
	int rc;

	assert(transport != NULL);
	if (transport->ops.ctrlr_get_reg_4_async == NULL) {
		rc = transport->ops.ctrlr_get_reg_4(ctrlr, offset, &value);
                                  /* [한국어] 동기로 먼저 읽음 */
		if (rc != 0) {
			return rc;
		}

		return nvme_queue_register_operation_completion(ctrlr, value, cb_fn, cb_arg);
                                  /* [한국어] 읽은 값을 ctx->value에 담아 콜백에 전달 */
	}

	return transport->ops.ctrlr_get_reg_4_async(ctrlr, offset, cb_fn, cb_arg);
}

int
nvme_transport_ctrlr_get_reg_8_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				     spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);
	uint64_t value;
                                  /* [한국어] 64비트 버전 — 주로 CAP 읽기 */
	int rc;

	assert(transport != NULL);
	if (transport->ops.ctrlr_get_reg_8_async == NULL) {
		rc = transport->ops.ctrlr_get_reg_8(ctrlr, offset, &value);
		if (rc != 0) {
			return rc;
		}

		return nvme_queue_register_operation_completion(ctrlr, value, cb_fn, cb_arg);
	}

	return transport->ops.ctrlr_get_reg_8_async(ctrlr, offset, cb_fn, cb_arg);
}

/*
 * [한국어] nvme_transport_ctrlr_get_max_xfer_size - 한 I/O의 최대 바이트 수 조회.
 *
 * PCIe는 보통 identify.MDTS(Maximum Data Transfer Size) × CAP.MPSMIN 페이지 크기로 결정.
 * Fabrics는 트랜스포트 프레임 크기와 MDTS의 min.
 * 상위(nvme_ns_cmd.c의 split 판정)가 이 값을 기준으로 요청을 분할.
 */
uint32_t
nvme_transport_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_get_max_xfer_size(ctrlr);
                                  /* [한국어] mandatory op — 모든 트랜스포트가 구현 */
}

/*
 * [한국어] nvme_transport_ctrlr_get_max_sges - 한 명령의 최대 SGE(Scatter-Gather Entry) 개수.
 *
 * PCIe: 호스트 메모리 페이지/PRP 제약으로 결정 (UINT16_MAX가 일반적).
 * RDMA: QP의 `max_send_sge`(보통 16) 또는 SGL scheme에 따라.
 * TCP: 4K PDU 단위 제약.
 *
 * 상위(nvme_ns_cmd.c)가 payload의 SGE 개수가 이 값을 넘으면 split.
 */
uint16_t
nvme_transport_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	return transport->ops.ctrlr_get_max_sges(ctrlr);
                                  /* [한국어] mandatory op */
}

/*
 * [한국어] ★ CMB(Controller Memory Buffer) API 3종 — PCIe NVMe 장치 내부 메모리 활용 ★
 *
 * CMB는 장치 내부의 메모리 공간으로, 호스트 메모리 대신 SQ/CQ/PRP 리스트/데이터 버퍼를 여기에 두면
 * PCIe DMA bounce 없이 장치가 직접 접근 → 레이턴시 감소, 대역 효율.
 *
 * 호출 시퀀스:
 *   reserve_cmb(ctrlr)                 — CMB 사용 예약 (내부 플래그 세팅)
 *   ptr = map_cmb(ctrlr, &size)        — BAR2/4에 mmap하여 유저스페이스 포인터 반환
 *   ...CMB 영역에 SQ/데이터 배치 + 사용...
 *   unmap_cmb(ctrlr)                   — mmap 해제
 *
 * 트랜스포트 지원:
 *   - PCIe: CMBSZ/CMBLOC 레지스터가 있으면 지원 (모든 NVMe PCIe 장치가 CMB를 갖진 않음).
 *   - RDMA/TCP: CMB 개념 없음 → -ENOTSUP / NULL.
 *
 * 사용처: SPDK NVMe hello_world 예제의 `CMB_SGL`, 고성능 라이브러리(BaM 등).
 */
int
nvme_transport_ctrlr_reserve_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.ctrlr_reserve_cmb != NULL) {
                                  /* [한국어] optional — PCIe 중 CMB 지원 장치만 구현 */
		return transport->ops.ctrlr_reserve_cmb(ctrlr);
	}

	return -ENOTSUP;
                                  /* [한국어] 미지원 트랜스포트/장치 */
}

void *
nvme_transport_ctrlr_map_cmb(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.ctrlr_map_cmb != NULL) {
		return transport->ops.ctrlr_map_cmb(ctrlr, size);
                                  /* [한국어] 매핑된 유저스페이스 포인터 반환, *size에 크기 기록 */
	}

	return NULL;
                                  /* [한국어] 매핑 실패/미지원 */
}

int
nvme_transport_ctrlr_unmap_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.ctrlr_unmap_cmb != NULL) {
		return transport->ops.ctrlr_unmap_cmb(ctrlr);
	}

	return 0;
                                  /* [한국어] 미지원이면 성공(0) — 매핑 안 했으니 해제할 것도 없음 */
}

/*
 * [한국어] ★ PMR(Persistent Memory Region) API 4종 — 장치 내부 비휘발성 메모리 ★
 *
 * PMR은 NVMe 1.4에서 추가된 persistent memory region (주로 Optane 기반 SSD).
 * CMB와 유사하게 BAR로 노출되지만 "비휘발성"이 핵심 — 전원 차단 후에도 내용 보존.
 *
 * 호출 시퀀스:
 *   enable_pmr(ctrlr)                   — PMRCTL.EN=1 설정 (전원 + 메모리 활성화)
 *   ptr = map_pmr(ctrlr, &size)         — BAR mmap
 *   ...비휘발성 데이터 저장...
 *   unmap_pmr(ctrlr)                    — mmap 해제
 *   disable_pmr(ctrlr)                  — PMRCTL.EN=0 (전력 절약)
 *
 * 차이점 from CMB: 초기화 과정에 PMRCTL/PMRSTS 레지스터 통신 필요, 전원 관리 의미 있음.
 *
 * 미지원 리턴값이 CMB와 다름: PMR은 -ENOSYS ("system call이 없음" — NVMe 1.3 이하 장치), CMB는 -ENOTSUP.
 */
int
nvme_transport_ctrlr_enable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.ctrlr_enable_pmr != NULL) {
		return transport->ops.ctrlr_enable_pmr(ctrlr);
	}

	return -ENOSYS;
                                  /* [한국어] PMR 미지원 */
}

int
nvme_transport_ctrlr_disable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.ctrlr_disable_pmr != NULL) {
		return transport->ops.ctrlr_disable_pmr(ctrlr);
	}

	return -ENOSYS;
}

void *
nvme_transport_ctrlr_map_pmr(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.ctrlr_map_pmr != NULL) {
		return transport->ops.ctrlr_map_pmr(ctrlr, size);
	}

	return NULL;
}

int
nvme_transport_ctrlr_unmap_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.ctrlr_unmap_pmr != NULL) {
		return transport->ops.ctrlr_unmap_pmr(ctrlr);
	}

	return -ENOSYS;
                                  /* [한국어] CMB의 unmap과 달리 미지원 시 -ENOSYS (호출자가 의도적 PMR 사용) */
}

/*
 * [한국어] ★ nvme_transport_ctrlr_create_io_qpair - I/O qpair 생성 + qpair->transport 캐시 저장 ★
 *
 * @qid:  원하는 SQ/CQ ID (사용자 지정 또는 자동). 0은 admin이므로 ≥1.
 * @opts: io_queue_size, qprio, async 등 큐 파라미터.
 * @return: 생성된 qpair*, 실패 시 NULL.
 *
 * 중요 포인트: 성공 시 `qpair->transport = transport`를 저장 — 이게 hot-path에서
 * `qpair->transport->ops.xxx`를 직접 호출하게 해주는 캐시. Admin 큐는 이 캐시를 안 씀.
 *
 * 호출자: `spdk_nvme_ctrlr_alloc_io_qpair` (공개 API).
 */
struct spdk_nvme_qpair *
nvme_transport_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				     const struct spdk_nvme_io_qpair_opts *opts)
{
	struct spdk_nvme_qpair *qpair;
                                  /* [한국어] 생성된 qpair */
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	qpair = transport->ops.ctrlr_create_io_qpair(ctrlr, qid, opts);
                                  /* [한국어] 트랜스포트별 구현 — PCIe는 SQ/CQ 메모리 할당, RDMA는 QP 생성 등 */
	if (qpair != NULL && !nvme_qpair_is_admin_queue(qpair)) {
                                  /* [한국어] I/O qpair만 transport 캐시 — admin은 multi-process 공유 금지 */
		qpair->transport = transport;
                                  /* [한국어] hot-path용 포인터 저장 — submit/process_completions에서 직접 사용 */
	}

	return qpair;
}

/*
 * [한국어] nvme_transport_ctrlr_delete_io_qpair - I/O qpair 해제 (create의 짝).
 *
 * 영문 주석이 핵심 설명: `qpair->transport`를 믿지 말고 반드시 `nvme_get_transport`로 다시 조회.
 * 이유: multi-process 환경에서 **다른 프로세스**가 이 I/O qpair를 삭제할 수 있는데,
 * 그 프로세스에서는 `qpair->transport` 포인터가 자기 주소 공간에서는 무효할 수 있다.
 * (각 프로세스가 자신의 g_transports[]에 별도 객체를 등록하므로)
 * → 지금 이 프로세스의 트랜스포트 객체를 확실히 찾기 위해 매번 lookup.
 *
 * 실패 시 assert — 정상 경로에서는 실패하지 않아야 함. 실패는 구현 버그.
 */
void
nvme_transport_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);
	int rc;

	assert(transport != NULL);

	/* Do not rely on qpair->transport.  For multi-process cases, a foreign process may delete
	 * the IO qpair, in which case the transport object would be invalid (each process has their
	 * own unique transport objects since they contain function pointers).  So we look up the
	 * transport object in the delete_io_qpair case.
	 */
	rc = transport->ops.ctrlr_delete_io_qpair(ctrlr, qpair);
                                  /* [한국어] 트랜스포트별 해제 — SQ/CQ 메모리 free, RDMA QP destroy 등 */
	if (rc != 0) {
                                  /* [한국어] 정상 경로에서는 0만 반환 — 실패는 assert로 fail-fast */
		NVME_CTRLR_ERRLOG(ctrlr, "transport %s returned non-zero for ctrlr_delete_io_qpair op\n",
				  transport->ops.name);
		assert(false);
	}
}

/*
 * [한국어] nvme_transport_connect_qpair_fail - connect 실패 시 실패 사유 복원 + disconnect.
 *
 * 호출자: `nvme_transport_ctrlr_connect_qpair`의 에러 경로, async poll group의 disconnected_qpair_cb.
 *
 * 기능:
 *   1) connect 시도 전에 저장해뒀던 `last_transport_failure_reason`을 현재 값으로 복원
 *      (connect 시도가 실패 사유를 덮어씌웠을 수 있기 때문에 원래 이유 보존)
 *   2) disconnect 절차 실행 → qpair 상태 정돈
 */
static void
nvme_transport_connect_qpair_fail(struct spdk_nvme_qpair *qpair, void *unused)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
                                  /* [한국어] qpair 소속 ctrlr 포인터 */

	/* If the qpair was unable to reconnect, restore the original failure reason */
	qpair->transport_failure_reason = qpair->last_transport_failure_reason;
                                  /* [한국어] 이전 실패 사유 복원 */
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, qpair);
                                  /* [한국어] disconnect 수행 — DISCONNECTING→DISCONNECTED 전이 트리거 */
}

/*
 * [한국어] ★★ nvme_transport_ctrlr_connect_qpair - qpair 연결 (상태머신 DISCONNECTED→CONNECTED) ★★
 *
 * I/O qpair 또는 admin qpair를 장치와 연결. 동기/비동기 모드에 따라 busy-wait 여부가 다름.
 *
 * 동작:
 *   1) I/O qpair이면서 `qpair->transport`가 NULL이면 (create 경로 아닌 첫 connect) 캐시 저장
 *   2) 이전 실패 사유 백업 + 현재 실패 사유 초기화 (NONE)
 *   3) state를 CONNECTING으로 전이
 *   4) 트랜스포트별 connect 실행
 *      - PCIe: SQ/CQ 생성 Admin 명령(Create I/O SQ/CQ) 제출
 *      - RDMA: CM connect + QP state transition + fabric CONNECT capsule
 *      - TCP: socket connect + ICReq/ICResp + fabric CONNECT capsule
 *   5) poll_group에 속해 있으면 poll_group_connect_qpair 호출 (connected 리스트로 이동)
 *   6) sync 모드면 state==CONNECTING인 동안 busy-wait:
 *      - fabrics + poll_group이면 poll_group 단위 완료 폴링
 *      - 그 외엔 qpair 단일 완료 폴링
 *      - 에러 발생 시 err 경로로
 *   7) async 모드면 호출자가 나중에 폴링하도록 바로 0 반환
 *
 * 에러 처리 (err 레이블):
 *   - `connect_qpair_fail`로 실패 사유 복원 + disconnect 트리거
 *   - async인데 disconnect도 async면 0 반환 (호출자가 폴링해서 DISCONNECTED 확인)
 *   - 그 외엔 rc(에러 코드) 반환
 */
int
nvme_transport_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);
	int rc;

	assert(transport != NULL);
	if (!nvme_qpair_is_admin_queue(qpair) && qpair->transport == NULL) {
                                  /* [한국어] I/O qpair + 캐시 미세팅(전에 delete 후 재사용된 qpair 등) → 세팅 */
		qpair->transport = transport;
	}

	qpair->last_transport_failure_reason = qpair->transport_failure_reason;
                                  /* [한국어] 이전 실패 사유 백업 (connect 시도가 덮어쓸 수 있으므로) */
	qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_NONE;
                                  /* [한국어] 현재 사유는 NONE으로 초기화 — 이번 시도에 성공 시 이 값 유지 */

	nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTING);
                                  /* [한국어] 상태를 CONNECTING으로 전이 — submit_request가 FABRIC CONNECT capsule만 허용 */
	rc = transport->ops.ctrlr_connect_qpair(ctrlr, qpair);
                                  /* [한국어] 트랜스포트별 connect 구현 호출 */
	if (rc != 0) {
                                  /* [한국어] connect 동기 실패 → err 경로 */
		goto err;
	}

	if (qpair->poll_group) {
                                  /* [한국어] poll_group에 가입되어 있으면 connect 완료 후 추가 처리 */
		rc = nvme_poll_group_connect_qpair(qpair);
                                  /* [한국어] poll_group의 connected 리스트로 이동 + 내부 상태 동기화 */
		if (rc) {
			goto err;
		}
	}

	if (!qpair->async) {
                                  /* [한국어] 동기 모드 — CONNECTING을 빠져나올 때까지 busy-wait */
		/* Busy wait until the qpair exits the connecting state */
		while (nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING) {
			if (qpair->poll_group && spdk_nvme_ctrlr_is_fabrics(ctrlr)) {
                                  /* [한국어] fabrics + poll_group이면 group 단위 폴링
                                   *         (여러 qpair의 connect가 동시에 진행 중일 수 있음) */
				rc = spdk_nvme_poll_group_process_completions(
					     qpair->poll_group->group, 0,
					     nvme_transport_connect_qpair_fail);
                                  /* [한국어] max=0 (무제한), 실패 qpair 처리 콜백 전달 */
			} else {
                                  /* [한국어] 그 외(PCIe 또는 poll_group 없음): 이 qpair 단일 폴링 */
				rc = spdk_nvme_qpair_process_completions(qpair, 0);
			}

			if (rc < 0) {
                                  /* [한국어] 폴링 자체 실패 — connect 실패로 처리 */
				goto err;
			}
		}
	}

	return 0;
                                  /* [한국어] sync 성공 또는 async 시작 성공 */
err:
	nvme_transport_connect_qpair_fail(qpair, NULL);
                                  /* [한국어] 실패 사유 복원 + disconnect */
	if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
                                  /* [한국어] disconnect도 비동기로 진행 중 — 호출자가 나중에 폴링해서 완료 확인해야 함 */
		assert(qpair->async == true);
                                  /* [한국어] 이 분기는 async 전용 */
		/* Let the caller to poll the qpair until it is actually disconnected. */
		return 0;
                                  /* [한국어] 0 반환 — 호출자가 폴링으로 DISCONNECTED 확인하게 함 */
	}

	return rc;
                                  /* [한국어] 이미 disconnect 완료 or sync 경로 — 에러 코드 전달 */
}

/*
 * [한국어] nvme_transport_ctrlr_disconnect_qpair - qpair 연결 해제 (CONNECTED→DISCONNECTING).
 *
 * 호출자: reset 중 qpair_check_enabled, 명시적 disconnect API, error path.
 *
 * 동작:
 *   1) 이미 DISCONNECTING/DISCONNECTED면 조기 리턴 (idempotent)
 *   2) 상태를 DISCONNECTING으로 전이
 *   3) poll_group 소속 + 현재 프로세스 소유 qpair이면 먼저 poll_group에서 disconnect
 *      (connected → disconnected 리스트 이동)
 *   4) 트랜스포트별 disconnect 실행 (동기든 비동기든 ops.ctrlr_disconnect_qpair가 완료시킴)
 */
void
nvme_transport_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING ||
	    nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTED) {
                                  /* [한국어] 이미 disconnect 진행/완료 상태 — idempotent 반환 */
		return;
	}

	nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTING);
                                  /* [한국어] 상태 전이 */
	assert(transport != NULL);

	if (qpair->poll_group && (qpair->active_proc == nvme_ctrlr_get_current_process(ctrlr))) {
                                  /* [한국어] poll_group 소속 + 이 프로세스가 active_proc이면
                                   *         poll_group의 connected 리스트에서 빼내 disconnected로 이동 */
		nvme_poll_group_disconnect_qpair(qpair);
	}

	transport->ops.ctrlr_disconnect_qpair(ctrlr, qpair);
                                  /* [한국어] 트랜스포트별 disconnect — SQ/CQ 삭제 admin 명령, RDMA disconnect 등
                                   *         완료되면 disconnect_qpair_done으로 DISCONNECTED 전이 */
}

/*
 * [한국어] nvme_transport_qpair_get_fd - 인터럽트 모드용 fd 조회 (optional op).
 *
 * 호출자: `spdk_nvme_qpair_get_fd` (nvme_qpair.c 공개 API).
 * 미지원 트랜스포트는 -ENOTSUP (polled-mode만 가능).
 */
int
nvme_transport_qpair_get_fd(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
			    struct spdk_event_handler_opts *opts)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.qpair_get_fd != NULL) {
                                  /* [한국어] optional — PCIe VFIO, TCP socket 정도만 구현 */
		return transport->ops.qpair_get_fd(qpair, opts);
	}

	return -ENOTSUP;
}

/*
 * [한국어] nvme_transport_ctrlr_disconnect_qpair_done - disconnect 완료 후처리 (상태머신의 마지막 훅).
 *
 * 호출자: 트랜스포트 disconnect 구현이 모든 정리를 마치고 마지막에 호출. 이 함수가 상태를 DISCONNECTED로 확정.
 *
 * 동작:
 *   1) active_proc == 현재 프로세스 OR admin qpair인 경우에만 queued_reqs abort
 *      (다른 프로세스 I/O qpair면 그 프로세스가 자기 abort 수행)
 *   2) 상태를 DISCONNECTED로 확정
 *   3) 인터럽트 모드 poll_group이면 disconnect fd에 이벤트 write — fgrp_wait가 깨어나 처리 가능하도록
 *   4) FABRIC 명령이 outstanding이면 fabric 측 정리 (poll/auth cleanup)
 */
void
nvme_transport_ctrlr_disconnect_qpair_done(struct spdk_nvme_qpair *qpair)
{
	if (qpair->active_proc == nvme_ctrlr_get_current_process(qpair->ctrlr) ||
	    nvme_qpair_is_admin_queue(qpair)) {
                                  /* [한국어] 이 I/O qpair의 소유 프로세스가 나 자신이거나, admin qpair인 경우만 abort */
		nvme_qpair_abort_all_queued_reqs(qpair);
                                  /* [한국어] queued_req/err_req_head/aborting 전부 abort 처리 */
	}
	nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
                                  /* [한국어] 상태 확정 */

	/* In interrupt mode qpairs that are added to poll group need an event for the
	 * disconnected qpairs handling to kick in.
	 */
	if (qpair->poll_group) {
                                  /* [한국어] interrupt-mode poll_group의 disconnect fd에 1바이트 write —
                                   *         fd_group_wait(epoll)가 깨어나 check_disconnected_qpairs 실행하도록 */
		nvme_poll_group_write_disconnect_qpair_fd(qpair->poll_group->group);
	}

	/* A Fabric command may be outstanding before a disconnect was invoked. */
	if (qpair->fabric_poll_status && !(qpair->auth.flags.in_auth_poll || qpair->in_connect_poll)) {
                                  /* [한국어] 아직 완료 안 된 fabric 명령이 있고, auth/connect 폴 중이 아니면 정리 */
		nvme_fabric_qpair_poll_cleanup(qpair);
                                  /* [한국어] fabric property get/set poll 상태 해제 */
		nvme_fabric_qpair_auth_cleanup(qpair, -ECANCELED);
                                  /* [한국어] 인증 진행 중이었다면 -ECANCELED로 완료 */
	}
}

/*
 * [한국어] nvme_transport_ctrlr_get_memory_domains - 트랜스포트가 지원하는 DMA 메모리 도메인 조회.
 *
 * @domains: 반환 배열
 * @array_size: 배열 크기. 트랜스포트가 더 많은 도메인을 지원하면 array_size만큼만 채우고 실제 수 반환.
 * @return: 도메인 개수 (미구현이면 0)
 *
 * 용도: 상위 레이어(bdev/accel)가 DMA-safe 메모리 풀을 결정할 때 참조.
 * 예: RDMA 트랜스포트는 특정 NIC의 MR(Memory Region) 도메인만 지원.
 */
int
nvme_transport_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
					struct spdk_memory_domain **domains, int array_size)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	assert(transport != NULL);
	if (transport->ops.ctrlr_get_memory_domains) {
                                  /* [한국어] optional — 지원하는 트랜스포트만 */
		return transport->ops.ctrlr_get_memory_domains(ctrlr, domains, array_size);
	}

	return 0;
                                  /* [한국어] 미지원 → 도메인 없음(0) */
}

/*
 * [한국어] nvme_transport_ctrlr_process_transport_events - 트랜스포트별 이벤트 큐 처리 (lock 보호).
 *
 * 호출자: `spdk_nvme_qpair_process_completions`의 admin 큐 전용 선행 작업 (nvme_qpair.c:line 841).
 *
 * 역할: hotplug 알림, RDMA CM 이벤트, TCP socket 에러 등 트랜스포트가 비동기적으로 받은 이벤트를 처리.
 *
 * 동기화: ctrlr_lock으로 감쌈 — multi-process에서 이벤트 처리 중 경쟁 방지.
 * Optional — 이벤트 시스템 없는 트랜스포트는 미구현 (0 반환).
 */
int
nvme_transport_ctrlr_process_transport_events(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);
	int rc = 0;

	assert(transport != NULL);
	if (transport->ops.ctrlr_process_transport_events) {
                                  /* [한국어] 이벤트 처리 op 구현된 경우만 */
		nvme_ctrlr_lock(ctrlr);
                                  /* [한국어] multi-process 공유 자료구조 보호 */
		rc = transport->ops.ctrlr_process_transport_events(ctrlr);
                                  /* [한국어] 트랜스포트별 이벤트 처리 (예: RDMA CM queue, hotplug netlink) */
		nvme_ctrlr_unlock(ctrlr);
	}

	return rc;
}

/*
 * [한국어] ★★ qpair hot-path 디스패치 공통 패턴 ★★
 *
 * 아래 5개 함수(abort_reqs, reset, submit_request, process_completions, iterate_requests)는
 * 모두 동일한 구조를 공유한다:
 *
 *     if (likely(!is_admin_queue(qpair)))
 *         return qpair->transport->ops.xxx(qpair, ...);   // I/O 경로: 캐시 포인터 직접 사용
 *     else
 *         return nvme_get_transport(trstring)->ops.xxx(qpair, ...);  // admin 경로: 매번 lookup
 *
 * 왜 이런 패턴인가:
 *   - **I/O qpair**는 단일 프로세스 전용 — `qpair->transport` 캐시가 항상 유효하다.
 *     spdk_likely로 분기 예측을 I/O 경로에 유리하게 (hot path).
 *   - **Admin qpair**는 primary/secondary 프로세스 공유 가능 — 현재 프로세스의
 *     트랜스포트 객체를 매번 trstring으로 찾아야 함.
 *
 * 이 5개 함수가 SPDK NVMe I/O hot path의 실질적 디스패치 지점이다.
 */

/*
 * [한국어] nvme_transport_qpair_abort_reqs - qpair에 대기 중인 모든 요청 abort.
 *
 * 호출자: nvme_qpair.c의 `check_enabled`(PCIe reset 경로), `process_completions`(is_removed 시),
 *         `abort_all_queued_reqs` 등.
 * abort_dnr 플래그 전달 — 호출자가 재시도 허용 여부 결정.
 */
void
nvme_transport_qpair_abort_reqs(struct spdk_nvme_qpair *qpair)
{
	const struct spdk_nvme_transport *transport;
                                  /* [한국어] admin 경로에서만 로컬 변수 사용 */

	if (spdk_likely(!nvme_qpair_is_admin_queue(qpair))) {
                                  /* [한국어] ★ I/O hot path — 캐시 포인터로 직접 호출 */
		qpair->transport->ops.qpair_abort_reqs(qpair, qpair->abort_dnr);
	} else {
                                  /* [한국어] Admin 경로 — 매번 lookup */
		transport = nvme_get_transport(qpair->ctrlr->trid.trstring);
		assert(transport != NULL);
		transport->ops.qpair_abort_reqs(qpair, qpair->abort_dnr);
	}
}

/*
 * [한국어] nvme_transport_qpair_reset - qpair 내부 상태 리셋 (트랜스포트별).
 *
 * 일반적인 ctrlr reset과 달리 qpair 자체의 카운터(sq_tail, cq_head, phase bit 등)를 초기화.
 * 호출자: ctrlr reset 중 I/O qpair 재사용할 때.
 */
int
nvme_transport_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	const struct spdk_nvme_transport *transport;

	if (spdk_likely(!nvme_qpair_is_admin_queue(qpair))) {
                                  /* [한국어] I/O hot path */
		return qpair->transport->ops.qpair_reset(qpair);
	}

	transport = nvme_get_transport(qpair->ctrlr->trid.trstring);
	assert(transport != NULL);
	return transport->ops.qpair_reset(qpair);
                                  /* [한국어] Admin 경로 */
}

/*
 * [한국어] ★★★ nvme_transport_qpair_submit_request — I/O 제출 디스패치의 최종 관문 ★★★
 *
 * nvme_qpair.c의 `_nvme_qpair_submit_request`가 nvme_request를 ENABLED qpair에 제출할 때 호출.
 * 이 함수가 반환하면 request는 **트랜스포트의 소유**가 됨 (SQ에 이미 기록되었거나 내부 큐에 이관).
 *
 * 실제 구현(qpair_submit_request):
 *   - PCIe: tracker 할당 → PRP/SGL 빌드 → SQ[tail]=SQE → doorbell MMIO
 *   - RDMA: WR(Work Request) 빌드 → ibv_post_send (장치가 나중에 capsule 전송)
 *   - TCP:  PDU 빌드 → 소켓 write (또는 send queue에 enqueue)
 *
 * 반환값:
 *   - 0: 성공 (완료는 나중에 process_completions가 수확)
 *   - -ENOMEM: 트래커/송신버퍼 고갈 — 상위가 EAGAIN으로 변환해 queued_req에 enqueue
 *   - -ENXIO: 연결 끊김 — abort 처리
 */
int
nvme_transport_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	const struct spdk_nvme_transport *transport;

	if (spdk_likely(!nvme_qpair_is_admin_queue(qpair))) {
                                  /* [한국어] ★ 가장 빈번한 경로 — I/O hot path는 캐시로 1회 간접 호출 */
		return qpair->transport->ops.qpair_submit_request(qpair, req);
	}

	transport = nvme_get_transport(qpair->ctrlr->trid.trstring);
	assert(transport != NULL);
	return transport->ops.qpair_submit_request(qpair, req);
                                  /* [한국어] Admin 경로 (덜 빈번) */
}

/*
 * [한국어] ★★★ nvme_transport_qpair_process_completions — 완료 폴링 디스패치의 최종 관문 ★★★
 *
 * nvme_qpair.c의 `spdk_nvme_qpair_process_completions`가 CQ에서 CQE를 수확할 때 호출.
 *
 * 실제 구현(qpair_process_completions):
 *   - PCIe: CQ[cq_head].phase 체크 → phase bit가 현재 phase와 일치하면 완료 엔트리
 *           → nvme_tracker 찾아서 nvme_request 복원 → nvme_complete_request 호출
 *   - RDMA: ibv_poll_cq로 Work Completion 드레인 → capsule 파싱 → request 복원
 *   - TCP:  소켓에서 PDU 수신 → capsule 파싱 → request 복원
 *
 * @max_completions: 이번 호출에서 수확할 최대 개수 (0=무제한 드라이버 기본값).
 * @return: 실제 수확한 개수 (양수), 에러 시 음수 errno.
 */
int32_t
nvme_transport_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	const struct spdk_nvme_transport *transport;

	if (spdk_likely(!nvme_qpair_is_admin_queue(qpair))) {
                                  /* [한국어] ★ 가장 뜨거운 호출지점 — reactor poller가 초당 수만 번 실행 */
		return qpair->transport->ops.qpair_process_completions(qpair, max_completions);
	}

	transport = nvme_get_transport(qpair->ctrlr->trid.trstring);
	assert(transport != NULL);
	return transport->ops.qpair_process_completions(qpair, max_completions);
}

/*
 * [한국어] nvme_transport_qpair_iterate_requests - outstanding request 순회 콜백 적용.
 *
 * @iter_fn: 각 request마다 호출될 콜백. 반환값 !=0 이면 순회 중단.
 * @arg: 콜백에 전달할 임의 컨텍스트
 *
 * 용도: timeout 체크, error injection 조사, debug dump 등.
 * 트랜스포트별 구현이 자기 tracker/outstanding 리스트를 순회한다.
 */
int
nvme_transport_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				      int (*iter_fn)(struct nvme_request *req, void *arg),
				      void *arg)
{
	const struct spdk_nvme_transport *transport;

	if (spdk_likely(!nvme_qpair_is_admin_queue(qpair))) {
		return qpair->transport->ops.qpair_iterate_requests(qpair, iter_fn, arg);
	}

	transport = nvme_get_transport(qpair->ctrlr->trid.trstring);
	assert(transport != NULL);
	return transport->ops.qpair_iterate_requests(qpair, iter_fn, arg);
}

/*
 * [한국어] nvme_transport_qpair_authenticate - NVMe-oF 인증 시작 (optional).
 *
 * NVMe-oF에서 DH-HMAC-CHAP 등 인증이 필요한 경우 이 함수로 시작.
 * PCIe는 인증 개념 없음 → -ENOTSUP.
 *
 * 주의: 여기서는 lookup을 `nvme_qpair_is_admin_queue` 분기 없이 한다 —
 *       인증은 connect 초기 단계라 qpair->transport 캐시가 아직 없을 수 있기 때문.
 */
int
nvme_transport_qpair_authenticate(struct spdk_nvme_qpair *qpair)
{
	const struct spdk_nvme_transport *transport;

	transport = nvme_get_transport(qpair->ctrlr->trid.trstring);
                                  /* [한국어] 인증 경로는 connect 초기라 항상 조회 */
	if (transport->ops.qpair_authenticate == NULL) {
                                  /* [한국어] optional — PCIe 등 미지원 */
		return -ENOTSUP;
	}

	return transport->ops.qpair_authenticate(qpair);
                                  /* [한국어] RDMA/TCP의 AUTH SEND/RECV capsule 교환 시작 */
}

/*
 * [한국어] nvme_transport_admin_qpair_abort_aers - outstanding AER(Async Event Request) abort.
 *
 * AER은 admin 큐에 상주하며 장치가 비동기 이벤트(미디어 에러, hotplug, SMART 경고 등)를 알리는 수단.
 * 컨트롤러 재시작/disconnect 시 outstanding AER을 모두 abort해야 깨끗한 상태가 됨.
 *
 * 호출자: ctrlr reset 경로, destroy 경로.
 * 모든 트랜스포트가 mandatory 구현.
 */
void
nvme_transport_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(qpair->ctrlr->trid.trstring);

	assert(transport != NULL);
	transport->ops.admin_qpair_abort_aers(qpair);
                                  /* [한국어] admin 큐만 AER을 가지므로 전용 함수 — I/O 큐에는 호출 안 함 */
}

/*
 * [한국어] ★★ 트랜스포트별 Poll Group 수명주기 및 qpair 이동 규약 ★★
 *
 * `spdk_nvme_transport_poll_group`은 **한 트랜스포트 내부**에서 여러 qpair의 CQ를 한 번에
 * 폴링하는 그룹 추상화. 상위 `nvme_poll_group`이 복수 트랜스포트별 tgroup을 묶어 관리함.
 *
 * qpair 이동 불변식: 한 qpair는 정확히 두 리스트 중 하나에만 속한다:
 *   - `tgroup->connected_qpairs`    (connect 성공 후 완료 폴링 대상)
 *   - `tgroup->disconnected_qpairs` (add 직후, 또는 disconnect 후)
 *
 * 이동 함수:
 *   - add:                     어디에도 없음 → disconnected_qpairs에 삽입
 *   - connect_qpair:           disconnected → connected
 *   - disconnect_qpair:        connected → disconnected
 *   - remove:                  disconnected → 어디에도 없음 (qpair->poll_group=NULL)
 *
 * `qpair->poll_group_tailq_head`가 현재 소속 리스트의 head 포인터를 가리키므로, 어느 쪽에
 * 있는지 O(1)로 판별 가능. `num_connected_qpairs`는 connected 리스트 길이 캐시.
 */

/*
 * [한국어] nvme_transport_poll_group_create - 트랜스포트별 poll group 객체 생성 + 공용 필드 초기화.
 */
struct spdk_nvme_transport_poll_group *
nvme_transport_poll_group_create(const struct spdk_nvme_transport *transport)
{
	struct spdk_nvme_transport_poll_group *group = NULL;
                                  /* [한국어] 트랜스포트가 할당한 구조체 (일반 구조체를 embed한 확장 형태) */

	group = transport->ops.poll_group_create();
                                  /* [한국어] 트랜스포트별 할당 — PCIe는 단순 구조체, RDMA는 ibv_comp_channel 포함 등 */
	if (group) {
                                  /* [한국어] 공용 필드 초기화 (트랜스포트별 구현은 자신만 알면 되는 확장 필드를 이미 설정) */
		group->transport = transport;
                                  /* [한국어] 백 포인터 — 이후 ops 호출에 사용 */
		STAILQ_INIT(&group->connected_qpairs);
                                  /* [한국어] 연결된 qpair 리스트 — 완료 폴링 대상 */
		STAILQ_INIT(&group->disconnected_qpairs);
                                  /* [한국어] 연결 끊긴 qpair 리스트 — check_disconnected_qpairs_cb 호출 대상 */
		group->num_connected_qpairs = 0;
                                  /* [한국어] connected 리스트 길이 카운터 (fastpath용 캐시) */
	}

	return group;
                                  /* [한국어] 할당 실패 시 NULL */
}

/*
 * [한국어] nvme_transport_poll_group_add - qpair를 tgroup의 disconnected 리스트에 추가.
 *
 * qpair는 아직 CONNECTED 상태 이전이어야 함(assert). add 시점에서는 아직 연결이 안 된 상태이며,
 * 이후 `connect_qpair`로 connected 리스트로 이동.
 */
int
nvme_transport_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			      struct spdk_nvme_qpair *qpair)
{
	int rc;

	rc = tgroup->transport->ops.poll_group_add(tgroup, qpair);
                                  /* [한국어] 트랜스포트별 add — RDMA는 QP를 group의 comp_channel에 연결 등 */
	if (rc == 0) {
		qpair->poll_group = tgroup;
                                  /* [한국어] qpair에 소속 기록 — nvme_qpair_check_enabled 등이 활용 */
		assert(nvme_qpair_get_state(qpair) < NVME_QPAIR_CONNECTED);
                                  /* [한국어] add는 아직 연결되지 않은 qpair에만 허용 */
		qpair->poll_group_tailq_head = &tgroup->disconnected_qpairs;
                                  /* [한국어] "현재 이 리스트에 있다" 마커 */
		STAILQ_INSERT_TAIL(&tgroup->disconnected_qpairs, qpair, poll_group_stailq);
                                  /* [한국어] disconnected 리스트 꼬리에 삽입 */
	}

	return rc;
}

/*
 * [한국어] nvme_transport_poll_group_remove - disconnected 리스트에 있는 qpair를 그룹에서 제거.
 *
 * 제약: connected 리스트에 있으면 -EINVAL, 둘 다 아니면 -ENOENT.
 * connected → disconnected 먼저 이동시킨 뒤 remove해야 한다.
 */
int
nvme_transport_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
				 struct spdk_nvme_qpair *qpair)
{
	int rc __attribute__((unused));
                                  /* [한국어] rc를 assert에서만 쓰므로 release 빌드 경고 회피 위한 unused 속성 */

	if (qpair->poll_group_tailq_head == &tgroup->connected_qpairs) {
                                  /* [한국어] 아직 connected 상태 — disconnect 먼저 필요 */
		return -EINVAL;
	} else if (qpair->poll_group_tailq_head != &tgroup->disconnected_qpairs) {
                                  /* [한국어] 어느 리스트에도 없음 — 이 그룹 소속이 아님 */
		return -ENOENT;
	}

	rc = tgroup->transport->ops.poll_group_remove(tgroup, qpair);
                                  /* [한국어] 트랜스포트별 해제 — comp_channel에서 분리 등 */
	assert(rc == 0);
                                  /* [한국어] remove op는 실패해선 안 됨 */

	STAILQ_REMOVE(&tgroup->disconnected_qpairs, qpair, spdk_nvme_qpair, poll_group_stailq);
                                  /* [한국어] 리스트에서 제거 */

	qpair->poll_group = NULL;
                                  /* [한국어] 소속 해제 */
	qpair->poll_group_tailq_head = NULL;
                                  /* [한국어] 리스트 소속 마커 해제 */

	return 0;
}

/*
 * [한국어] ★ nvme_transport_poll_group_process_completions - 그룹 단위 완료 폴링 (hot path) ★
 *
 * 그룹 내 **connected qpair들의 CQ를 한 번에 드레인** — 여러 qpair를 병렬 폴링할 때 큰 효과.
 *
 * @completions_per_qpair: qpair당 최대 처리 수 (0=무제한)
 * @disconnected_qpair_cb: 폴링 중 disconnect된 qpair 발견 시 호출할 콜백
 * @return: 총 처리한 완료 개수 (모든 qpair 합계)
 *
 * 실제 구현:
 *   - RDMA: 하나의 comp_channel에서 ibv_poll_cq — 여러 QP의 완료가 한 번에
 *   - TCP: epoll fd로 여러 socket을 한 번에 감시
 *   - PCIe: 순차 qpair 순회
 */
int64_t
nvme_transport_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	return tgroup->transport->ops.poll_group_process_completions(tgroup, completions_per_qpair,
			disconnected_qpair_cb);
                                  /* [한국어] 트랜스포트 구현으로 위임 — optional/mandatory 분기 불필요(상위가 항상 존재 가정) */
}

/*
 * [한국어] nvme_transport_poll_group_check_disconnected_qpairs - disconnected 리스트 주기적 확인.
 *
 * disconnected 리스트에 쌓인 qpair들을 주기적으로 정리 (사용자 콜백으로 재연결 시도 여부 결정).
 */
void
nvme_transport_poll_group_check_disconnected_qpairs(struct spdk_nvme_transport_poll_group *tgroup,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	return tgroup->transport->ops.poll_group_check_disconnected_qpairs(tgroup,
			disconnected_qpair_cb);
                                  /* [한국어] 각 disconnected qpair에 대해 콜백 호출 — 재연결 or remove 판단 */
}

/*
 * [한국어] nvme_transport_poll_group_destroy - poll group 해제 (create의 짝).
 * 리스트가 비어 있어야 안전 (호출자 책임).
 */
int
nvme_transport_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	return tgroup->transport->ops.poll_group_destroy(tgroup);
                                  /* [한국어] 트랜스포트별 해제 — RDMA comp_channel destroy 등 */
}

/*
 * [한국어] ★ nvme_transport_poll_group_disconnect_qpair - connected → disconnected 리스트 이동 ★
 *
 * 호출자: disconnect 경로의 `nvme_transport_ctrlr_disconnect_qpair`가 이 함수를 호출.
 *
 * 동작:
 *   1) 이미 disconnected면 idempotent 0 반환
 *   2) connected에 있으면:
 *      - 트랜스포트별 disconnect_qpair 호출 (QP state transition, comp_channel 분리 등)
 *      - connected에서 제거 + num_connected_qpairs-- + disconnected 꼬리에 삽입
 *   3) 둘 다 아니면 -EINVAL
 */
int
nvme_transport_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	int rc __attribute__((unused));

	tgroup = qpair->poll_group;
                                  /* [한국어] qpair에 저장된 poll_group 포인터 — NULL이면 이 함수 호출 자체가 오류 */

	if (qpair->poll_group_tailq_head == &tgroup->disconnected_qpairs) {
                                  /* [한국어] 이미 disconnected — idempotent */
		return 0;
	}

	if (qpair->poll_group_tailq_head == &tgroup->connected_qpairs) {
                                  /* [한국어] connected에 있음 → 이동 */
		rc = tgroup->transport->ops.poll_group_disconnect_qpair(qpair);
                                  /* [한국어] 트랜스포트별 disconnect 호출 */
		assert(rc == 0);

		qpair->poll_group_tailq_head = &tgroup->disconnected_qpairs;
                                  /* [한국어] 소속 마커 변경 */
		STAILQ_REMOVE(&tgroup->connected_qpairs, qpair, spdk_nvme_qpair, poll_group_stailq);
                                  /* [한국어] connected에서 제거 */
		assert(tgroup->num_connected_qpairs > 0);
                                  /* [한국어] 카운터 무결성 */
		tgroup->num_connected_qpairs--;
                                  /* [한국어] connected 수 감소 */
		STAILQ_INSERT_TAIL(&tgroup->disconnected_qpairs, qpair, poll_group_stailq);
                                  /* [한국어] disconnected 꼬리에 삽입 */

		return 0;
	}

	return -EINVAL;
                                  /* [한국어] 어느 리스트에도 없음 — 호출 오류 */
}

/*
 * [한국어] ★ nvme_transport_poll_group_connect_qpair - disconnected → connected 리스트 이동 ★
 *
 * disconnect의 역방향. connect 성공 시 connected 리스트로 이동 + num_connected_qpairs 증가.
 *
 * -EINPROGRESS 반환 시 0으로 변환 — 비동기 connect가 진행 중이라는 의미이므로 호출자 관점에서는 "시작됨"으로 해석.
 */
int
nvme_transport_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_transport_poll_group *tgroup;
	int rc;

	tgroup = qpair->poll_group;

	if (qpair->poll_group_tailq_head == &tgroup->connected_qpairs) {
                                  /* [한국어] 이미 connected — idempotent */
		return 0;
	}

	if (qpair->poll_group_tailq_head == &tgroup->disconnected_qpairs) {
                                  /* [한국어] disconnected에서 connected로 이동 */
		rc = tgroup->transport->ops.poll_group_connect_qpair(qpair);
                                  /* [한국어] 트랜스포트 connect op 호출 */
		if (rc == 0) {
			qpair->poll_group_tailq_head = &tgroup->connected_qpairs;
                                  /* [한국어] 소속 마커 변경 */
			STAILQ_REMOVE(&tgroup->disconnected_qpairs, qpair, spdk_nvme_qpair, poll_group_stailq);
                                  /* [한국어] disconnected에서 제거 */
			STAILQ_INSERT_TAIL(&tgroup->connected_qpairs, qpair, poll_group_stailq);
                                  /* [한국어] connected 꼬리에 삽입 */
			tgroup->num_connected_qpairs++;
                                  /* [한국어] 카운터 증가 */
		}

		return rc == -EINPROGRESS ? 0 : rc;
                                  /* [한국어] -EINPROGRESS(비동기 진행 중)는 정상으로 간주 — 호출자는 나중에 폴링 */
	}


	return -EINVAL;
                                  /* [한국어] 어느 리스트에도 없음 */
}

/*
 * [한국어] nvme_transport_poll_group_get_stats - 트랜스포트별 통계 조회 (optional).
 *
 * @stats: out — 트랜스포트가 할당한 통계 구조체 포인터 (free_stats로 해제 필요)
 * 용도: 애플리케이션이 각 트랜스포트의 내부 카운터(RDMA WR 수, TCP bytes 등) 확인.
 */
int
nvme_transport_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
				    struct spdk_nvme_transport_poll_group_stat **stats)
{
	if (tgroup->transport->ops.poll_group_get_stats) {
                                  /* [한국어] optional — 지원 트랜스포트만 */
		return tgroup->transport->ops.poll_group_get_stats(tgroup, stats);
	}
	return -ENOTSUP;
}

/*
 * [한국어] nvme_transport_poll_group_free_stats - get_stats의 짝 해제 함수.
 */
void
nvme_transport_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
				     struct spdk_nvme_transport_poll_group_stat *stats)
{
	if (tgroup->transport->ops.poll_group_free_stats) {
                                  /* [한국어] optional — get_stats 구현하면 free_stats도 짝으로 있어야 */
		tgroup->transport->ops.poll_group_free_stats(tgroup, stats);
	}
}

/*
 * [한국어] nvme_transport_get_trtype - 트랜스포트 객체에서 enum trtype 추출.
 *
 * 내부 유틸리티. SPDK_NVME_TRANSPORT_PCIE/RDMA/TCP/VFIOUSER/CUSE 중 하나 반환.
 */
spdk_nvme_transport_type_t
nvme_transport_get_trtype(const struct spdk_nvme_transport *transport)
{
	return transport->ops.type;
                                  /* [한국어] ops 구조체 내 type 필드 — 트랜스포트 등록 시 SPDK_NVME_TRANSPORT_PCIE 등으로 세팅 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_transport_get_opts - 전역 트랜스포트 설정 조회 (ABI 호환) ★
 *
 * opts_size 패턴(ABI forward/backward 호환):
 *   - 호출자는 자기가 빌드된 시점의 struct 크기를 opts_size로 전달.
 *   - 이 함수는 opts_size가 감당할 수 있는 필드까지만 채움.
 *   - 새 필드 추가 시: 테이블에 SET_FIELD 추가 + STATIC_ASSERT 크기 업데이트
 *     → 이전 SPDK로 빌드한 애플리케이션이 최신 SPDK 라이브러리와 링크돼도 안전.
 *
 * 반환: void — 에러는 SPDK_ERRLOG로만 알림. opts->opts_size에 실제 사용한 크기 저장.
 */
void
spdk_nvme_transport_get_opts(struct spdk_nvme_transport_opts *opts, size_t opts_size)
{
	if (opts == NULL) {
                                  /* [한국어] NULL 방어 */
		SPDK_ERRLOG("opts should not be NULL.\n");
		return;
	}

	if (opts_size == 0) {
                                  /* [한국어] 0도 방어 — 호출자가 sizeof 계산 실수 방지 */
		SPDK_ERRLOG("opts_size should not be zero.\n");
		return;
	}

	opts->opts_size = opts_size;
                                  /* [한국어] 호출자에게 "내가 이 크기까지 채웠다" 표시 */

#define SET_FIELD(field) \
	if (offsetof(struct spdk_nvme_transport_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = g_spdk_nvme_transport_opts.field; \
	} \
                                  /* [한국어] SET_FIELD 매크로 — offsetof+sizeof가 opts_size 이내면 채우고,
                                   *         초과하면 호출자 struct에 그 필드가 없으므로 skip. */

	SET_FIELD(rdma_srq_size);
                                  /* [한국어] RDMA Shared Receive Queue 크기 — 0이면 비활성, >0이면 trpid별 SRQ 사용 */
	SET_FIELD(rdma_max_cq_size);
                                  /* [한국어] RDMA CQ 최대 크기 — 0이면 자동 */
	SET_FIELD(rdma_cm_event_timeout_ms);
                                  /* [한국어] RDMA CM 이벤트 대기 최대 시간 (ms) — connect 실패 감지 */
	SET_FIELD(rdma_umr_per_io);
                                  /* [한국어] I/O마다 UMR(User-Mode Region) 등록 여부 */
	SET_FIELD(tcp_connect_timeout_ms);
                                  /* [한국어] TCP connect 타임아웃 (ms) — 0이면 SPDK 기본값 */

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_transport_opts) == 32, "Incorrect size");
                                  /* [한국어] 컴파일 타임 크기 검증 — 새 필드 추가 시 반드시 이 숫자도 갱신 */

#undef SET_FIELD
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_transport_set_opts - 전역 설정 변경 (ABI 호환) ★
 *
 * get_opts의 쓰기 버전. 호출자는 자기가 아는 필드만 채워 전달;
 * 이 함수는 **opts->opts_size**에 맞춰 해당 필드들만 반영 (이전 필드들은 그대로 유지).
 *
 * 입력 검증: tcp_connect_timeout_ms가 INT_MAX 초과 시 -EINVAL.
 *
 * 호출 타이밍: 트랜스포트 등록 **이전**에 호출해야 새 값이 반영 (등록 후에는 이미 캐시된 값 사용).
 * 주로 애플리케이션 초기화 시 `spdk_env_init` 직후 설정.
 */
int
spdk_nvme_transport_set_opts(const struct spdk_nvme_transport_opts *opts, size_t opts_size)
{
	if (opts == NULL) {
		SPDK_ERRLOG("opts should not be NULL.\n");
		return -EINVAL;
	}

	if (opts_size == 0) {
		SPDK_ERRLOG("opts_size should not be zero.\n");
		return -EINVAL;
	}

#define SET_FIELD(field) \
	if (offsetof(struct spdk_nvme_transport_opts, field) + sizeof(opts->field) <= opts->opts_size) { \
		g_spdk_nvme_transport_opts.field = opts->field; \
	} \
                                  /* [한국어] get과 방향만 반대 — 전역에 쓰기. 호출자 opts_size 기준. */

	SET_FIELD(rdma_srq_size);
	SET_FIELD(rdma_max_cq_size);
	SET_FIELD(rdma_cm_event_timeout_ms);
	SET_FIELD(rdma_umr_per_io);
	SET_FIELD(tcp_connect_timeout_ms);

	if (g_spdk_nvme_transport_opts.tcp_connect_timeout_ms > INT_MAX) {
                                  /* [한국어] 범위 검증 — poll()의 timeout 파라미터가 int이므로 INT_MAX 이하여야 */
		SPDK_ERRLOG("tcp_connect_timeout_ms opt cannot exceed INT_MAX\n");
		return -EINVAL;
	}

	g_spdk_nvme_transport_opts.opts_size = opts->opts_size;
                                  /* [한국어] 전역 opts_size 업데이트 — 다음 get_opts가 참조 */

#undef SET_FIELD

	return 0;
                                  /* [한국어] 성공 */
}

/*
 * [한국어] ★ 공개 API: spdk_nvme_ctrlr_get_registers - 사용자 코드용 레지스터 포인터 노출 ★
 *
 * @return: volatile 포인터 — 사용자가 레지스터 필드를 직접 읽을 수 있음 (MMIO-mapped 영역)
 *          - PCIe: BAR0 매핑 영역 (CAP/VS/CC/CSTS 등)
 *          - 일부 트랜스포트는 레지스터 추상화 지원 안 해 NULL 반환
 *
 * volatile이 중요한 이유: MMIO 읽기는 매번 실제 하드웨어에서 fetch되어야 하므로
 * 컴파일러 최적화(읽기 합침/캐싱)를 금지해야 함.
 *
 * 주의: 이 포인터를 통한 MMIO read는 PCIe link가 살아있어야 안전. ctrlr_is_removed=true면
 * 참조 시 seg fault 가능 — 상위가 상태를 확인해야 함.
 */
volatile struct spdk_nvme_registers *
spdk_nvme_ctrlr_get_registers(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport *transport = nvme_get_transport(ctrlr->trid.trstring);

	if (transport == NULL) {
		/* Transport does not exist. */
		return NULL;
                                  /* [한국어] 트랜스포트 미등록 — 방어적 NULL */
	}

	if (transport->ops.ctrlr_get_registers) {
                                  /* [한국어] optional — PCIe만 지원. Fabrics는 property get으로 간접 접근하므로 직접 포인터 노출 안 함. */
		return transport->ops.ctrlr_get_registers(ctrlr);
	}

	return NULL;
                                  /* [한국어] 미지원 트랜스포트 */
}
