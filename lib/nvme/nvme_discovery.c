/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 */

/*
 * [한국어 설명] NVMe-over-Fabrics Discovery Log Page 클라이언트 (nvme_discovery.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe-over-Fabrics (NVMe-oF) **Discovery Service** 클라이언트의 핵심 흐름을
 * 구현한다. NVMe-oF 환경에서 호스트는 먼저 "Discovery Controller" (ANN/ANY transport
 * 위에 NQN=`nqn.2014-08.org.nvmexpress.discovery`로 식별)에 연결하여
 * **Discovery Log Page (NVMe Log Identifier 0x70)** 를 읽고, 그 안의 entry 목록
 * (각 entry = 접속 가능한 subsystem NQN + transport address + service id)을 받아
 * 사용자가 실제 storage subsystem 에 connect 할 수 있게 한다. 이 파일이 제공하는 단일
 * public 진입점 `spdk_nvme_ctrlr_get_discovery_log_page()` 는 그 과정을 비동기 콜백
 * 체인으로 캡슐화한다:
 *
 *   1) Discovery 컨트롤러에서 처음 4KB만 fetch → 헤더의 `numrec`(entry 개수)를 보고
 *      필요한 전체 page 크기(`sizeof(header) + numrec * sizeof(entry)`)를 계산.
 *   2) buffer를 realloc 하여 "전체" log page 를 한 번 더 fetch (`get_log_page_completion`).
 *   3) Discovery 도중 controller 가 토폴로지 변화를 감지해 log를 갱신하면
 *      `genctr` (Generation Counter) 가 증가 → 본 클라이언트는 재요청 직전과 직후의
 *      genctr를 비교하여 변동이 있으면 처음부터 재시작 (`get_log_page_completion_final`).
 *      이 패턴이 NVMe-oF spec이 권장하는 atomic snapshot 보장 메커니즘이다.
 *   4) 최종적으로 사용자 cb_fn(cb_arg, rc, cpl, log_page) 호출 → 사용자는 entry들을
 *      순회하며 `spdk_nvme_connect`/`probe`로 실제 subsystem에 attach.
 *
 * **본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본**이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호스트 NVMe-oF 워크플로의 "Phase 1: 무엇이 있는지 알아보기" 단계. NVMe-oF 표준 흐름:
 *   (a) 사용자가 spdk_nvme_connect() 등으로 Discovery Controller에 연결
 *     → lib/nvme/nvme_fabric.c 의 nvme_fabric_ctrlr_discover() 등 트랜스포트 협상
 *   (b) 그 컨트롤러 핸들로 spdk_nvme_ctrlr_get_discovery_log_page() 호출 (이 파일!)
 *     → Discovery log page 받아서 사용자에게 entry 배열 반환
 *   (c) 사용자가 entry를 돌면서 각 subsystem에 spdk_nvme_connect() 추가 호출
 *
 * 호출 체인:
 *   사용자 (예: nvme bdev 모듈, app/spdk_nvme_discover, nvmf 자동 attach)
 *     → spdk_nvme_ctrlr_get_discovery_log_page (이 파일, public)
 *       → spdk_nvme_ctrlr_cmd_get_log_page (lib/nvme/nvme_ctrlr.c)
 *         → nvme_allocate_request_user_contig + admin queue submit
 *           → 트랜스포트(RDMA/TCP/...) 가 실제 NVMe Get Log Page (opcode 0x02) 송신
 *             → CQE → discovery_log_header_completion (이 파일)
 *               → spdk_nvme_ctrlr_cmd_get_log_page (재호출, 전체 buffer)
 *                 → CQE → get_log_page_completion (이 파일)
 *                   → spdk_nvme_ctrlr_cmd_get_log_page (재호출, genctr 만)
 *                     → CQE → get_log_page_completion_final (이 파일)
 *                       → 사용자 cb_fn(cb_arg, 0, cpl, log_page) — 또는 변경 감지 시 재시작
 *
 * 실행 컨텍스트: 모든 콜백은 admin qpair completion 폴링이 일어나는 SPDK thread에서
 * 호출된다. 이는 보통 컨트롤러 attach를 시작한 메인 스레드 또는 별도 admin poller 스레드.
 * 콜백 안에서 추가 admin 명령을 큐잉하는 패턴은 SPDK NVMe 드라이버 표준 — admin q는
 * 자기 자신에게 명령을 추가해도 안전하다 (queue depth 한계만 주의).
 *
 * === 타 모듈과의 연결 ===
 *  - **lib/nvme/nvme_internal.h** — `struct spdk_nvme_ctrlr`, `NVME_CTRLR_ERRLOG`,
 *    `spdk_nvmf_discovery_log_page` typedef 사용.
 *  - **include/spdk/nvme.h** — public 진입점 `spdk_nvme_ctrlr_get_discovery_log_page`
 *    의 prototype, 콜백 타입 `spdk_nvme_discovery_cb`.
 *  - **include/spdk/nvmf_spec.h** — `struct spdk_nvmf_discovery_log_page`
 *    (header: genctr/numrec/recfmt + entries[]), `struct spdk_nvmf_discovery_log_page_entry`
 *    (transport type, NQN, address, service id 등).
 *  - **lib/nvme/nvme_ctrlr.c** — `spdk_nvme_ctrlr_cmd_get_log_page` 사용. 일반 NVMe
 *    Get Log Page (opcode 0x02) 명령을 admin q로 보내는 헬퍼.
 *  - **include/spdk/endian.h** — `from_le16/from_le64` 매크로. NVMe spec은 little-endian
 *    이므로 host endian 변환 필요. (현 host가 LE면 사실상 no-op이지만 portability 보장.)
 *  - **lib/nvme/nvme_fabric.c** — Discovery 컨트롤러 자체의 fabric connect 처리 담당.
 *    본 파일과 협력하여 NVMe-oF 호스트 측의 "discovery 단계" 완성.
 *  - **사용자 측 모듈** — module/bdev/nvme/bdev_nvme.c (auto-attach), nvmf target의
 *    referral, app/spdk_nvme_discover 도구 등.
 *
 * === 주요 함수/구조체 요약 ===
 *  - `struct nvme_discovery_ctx` — 비동기 콜백 체인 사이에서 운반되는 상태:
 *    컨트롤러 핸들, 진행 중인 log_page buffer, 시작/종료 genctr, 사용자 cb_fn/cb_arg.
 *  - `spdk_nvme_ctrlr_get_discovery_log_page` — 외부 진입점. ctx 할당 + 첫 admin 명령 발사.
 *  - `discovery_log_header_completion` — 1차(헤더 4KB) 완료 콜백.
 *    numrec 보고 buffer 확장 + 전체 fetch 발사.
 *  - `get_log_page_completion` — 2차(전체) 완료 콜백.
 *    end_genctr 만 다시 받아오는 3차 명령 발사.
 *  - `get_log_page_completion_final` — 3차 완료 콜백. genctr 비교 → 변동 없으면
 *    사용자 cb_fn 호출, 변동 있으면 처음부터 재시도.
 *
 * === NVMe-oF Discovery 도메인 기초 ===
 *  - **Discovery NQN**: `nqn.2014-08.org.nvmexpress.discovery` — 모든 NVMe-oF 발견 서비스의
 *    표준 NQN. 호스트가 이 NQN으로 연결하면 Discovery Controller 가 응답.
 *  - **Discovery Log Page (LID = 0x70 = SPDK_NVME_LOG_DISCOVERY)**: 헤더(20+ 바이트) +
 *    entry 배열. 헤더의 `genctr`는 토폴로지가 바뀔 때마다 증가하는 monotonic counter.
 *  - **Atomic Snapshot 패턴**: 사용자가 큰 log page를 다 받는 동안 genctr가 증가하면
 *    중간에 entry가 추가/제거됐을 수 있으므로, 시작 직전 genctr와 끝난 직후 genctr를
 *    비교하여 같으면 신뢰, 다르면 재시도. 본 파일이 정확히 이 알고리즘을 구현.
 *  - **Persistent Discovery Controller (PDC)**: NVMe-oF 1.1+ 의 비동기 알림 모델.
 *    본 파일은 1회성 polling 모델이며 PDC 변경 알림은 다른 경로에서 처리.
 */

/* [한국어] nvme_internal.h: SPDK NVMe 내부 헤더 — ctrlr/admin command 헬퍼. */
#include "nvme_internal.h"

/* [한국어] spdk/endian.h: little-endian 인코딩된 NVMe 필드를 host endian으로 변환하는
 * from_le16/from_le32/from_le64 매크로 모음. NVMe spec 모든 정수 필드는 LE이므로 필수. */
#include "spdk/endian.h"

/*
 * [한국어]
 * struct nvme_discovery_ctx - Discovery 비동기 흐름을 가로지르는 상태 컨텍스트.
 *
 * 이 구조체는 spdk_nvme_ctrlr_get_discovery_log_page()에서 calloc되어 모든 admin 명령의
 * `cb_arg`로 전달되며, 콜백 체인의 마지막 단계(get_log_page_completion_final 또는 에러
 * 분기)에서 free된다. 따라서 콜백 사이에서 어떠한 stack-local 상태도 잃지 않고
 * 운반된다 — SPDK NVMe 드라이버 콜백 체인의 표준 패턴.
 */
struct nvme_discovery_ctx {
	struct spdk_nvme_ctrlr			*ctrlr;
	/* [한국어] Discovery 명령을 보낼 NVMe-oF Discovery Controller 핸들.
	 * 설정자: spdk_nvme_ctrlr_get_discovery_log_page() 입력 인자.
	 * 읽는 자: 모든 콜백이 추가 admin 명령 발사 시 ctrlr 인자로 사용.
	 * 값 범위: 유효한 NVMe-oF Discovery 컨트롤러 (사용자가 사전에 attach).
	 * 동기화: 단일 스레드(admin q owner) 컨텍스트라 락 불필요. */

	struct spdk_nvmf_discovery_log_page	*log_page;
	/* [한국어] 진행 중인 Discovery Log Page buffer. 처음에는 헤더 1개분(4KB 미만)으로
	 * 시작했다가 numrec 알게 되면 realloc()으로 전체 크기로 확장.
	 * 설정자: 진입점에서 calloc, header 콜백에서 realloc, 에러/완료 시 free.
	 * 읽는 자: 모든 콜백이 cpl 처리 후 next admin 명령의 buffer로 전달.
	 *           최종적으로 사용자 cb_fn(.., log_page) 로 전달.
	 * 값 범위: NULL 아님 (할당 실패 시 즉시 cb_fn(-ENOMEM) 호출하고 ctx 해제).
	 * 동기화: 단일 admin 스레드 소유 — 콜백 체인 안에서 한 번에 하나만 진행. */

	uint64_t				start_genctr;
	/* [한국어] 첫 fetch에서 받은 generation counter — atomic snapshot 검증의 기준값.
	 * 설정자: discovery_log_header_completion이 헤더 파싱 직후 저장.
	 * 읽는 자: get_log_page_completion_final이 end_genctr와 비교.
	 * 값 범위: 64bit monotonic — 컨트롤러 측이 토폴로지 변화 때마다 증가.
	 * 동기화: 콜백 체인 단일 스레드 — 단일 writer/reader. */

	uint64_t				end_genctr;
	/* [한국어] 전체 log page를 받은 후 다시 fetch한 genctr — start와 비교용.
	 * 설정자: get_log_page_completion이 두 번째 get_log_page 호출의 buffer로 지정해
	 *         디바이스가 직접 채움 (8 byte payload).
	 * 읽는 자: get_log_page_completion_final이 start_genctr와 비교.
	 * 값 범위: 64bit. start_genctr와 같으면 atomic snapshot 성공, 다르면 재시도.
	 * 동기화: 단일 admin 스레드. */

	spdk_nvme_discovery_cb			cb_fn;
	/* [한국어] 사용자 완료 콜백. 4-인자: (cb_arg, status_code, cpl_ptr, log_page_ptr).
	 * 설정자: 진입점 인자로 받아 ctx에 저장.
	 * 읽는 자: 모든 콜백 분기에서 종료 시 호출 (성공/에러/재시도 시작).
	 * 값 범위: 유효한 함수 포인터. NULL 검증은 spec 상 사용자 책임.
	 * 동기화: admin q owner 스레드에서 호출됨. */

	void					*cb_arg;
	/* [한국어] cb_fn에 전달할 사용자 컨텍스트.
	 * 설정자: 진입점 인자로 받아 ctx에 저장.
	 * 읽는 자: 모든 콜백 종료 분기.
	 * 값 범위: 임의 포인터 (NULL 가능, 사용자 의미 정의).
	 * 동기화: cb_fn 측에서 책임. */
};

/*
 * [한국어]
 * get_log_page_completion_final - 3차(최종 genctr 재조회) 완료 콜백.
 *
 * @cb_arg: nvme_discovery_ctx 포인터 (진입점에서 calloc).
 * @cpl: NVMe Completion Queue Entry — 3차 admin 명령의 결과.
 *
 * 이 콜백이 호출된 시점에는 ctx->log_page가 전체 entry로 채워져 있고, ctx->end_genctr
 * 도 디바이스가 직접 채워준 최신 generation counter 값을 담고 있다. 본 함수의 일은
 * "snapshot atomicity" 검증:
 *   - error CQE → 사용자 cb_fn에 cpl 전달, log_page free, ctx free.
 *   - start_genctr == end_genctr → 변화 없음, 신뢰 가능 → 사용자에게 log_page 전달.
 *   - start_genctr != end_genctr → 토폴로지 변경됨 → 처음부터 재시작
 *     (spdk_nvme_ctrlr_get_discovery_log_page 재호출, 기존 log_page 폐기).
 *
 * 실행 컨텍스트: admin qpair completion 폴링이 일어나는 SPDK thread. 콜백 안에서
 * 추가 admin 명령 큐잉(self-recursion via spdk_nvme_ctrlr_get_discovery_log_page) 가능.
 * 무한 재시작 가능성: 토폴로지가 매우 빠르게 바뀌는 환경에서는 무한 재시작이 이론적으로
 * 가능 — spec/구현 모두 명시적 retry-limit 없음. 운영상 자연 수렴 가정.
 *
 * 호출 체인:
 *   admin q completion poller → [이 함수] → 사용자 cb_fn 또는 spdk_nvme_ctrlr_get_discovery_log_page
 */
static void
get_log_page_completion_final(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_discovery_ctx *ctx = cb_arg; /* [한국어] cb_arg → ctx 캐스팅 (calloc된 컨텍스트). */
	int rc; /* [한국어] 재시작 시 새 admin 명령의 동기 반환값. */

	/* [한국어] CQE의 SC/SCT 필드에 에러가 있는지 SPDK helper로 검사. genctr 조회 자체가
	 * 실패한 경우 — 컨트롤러가 log page를 더 이상 지원 안 한다거나 transport 끊김. */
	if (spdk_nvme_cpl_is_error(cpl)) {
		free(ctx->log_page); /* [한국어] 진행 중이던 log page buffer 정리. */
		/* [한국어] 사용자에게 에러 보고: rc=0, cpl 전달(에러 정보 포함), log_page=NULL.
		 * rc=0인 이유는 SPDK 관례상 "비동기 NVMe 에러"는 cpl로, "동기 시스템 에러"는 rc로
		 * 분리 보고 — 여기서는 비동기 NVMe 에러이므로 rc=0. */
		ctx->cb_fn(ctx->cb_arg, 0, cpl, NULL);
		free(ctx); /* [한국어] 컨텍스트 해제로 라이프사이클 종료. */
		return;
	}

	/* Compare original genctr with latest genctr. If it changed, we need to restart. */
	/* [한국어] Atomic snapshot 검증: 시작 직전과 끝난 직후의 genctr가 같으면
	 * 이 사이에 토폴로지 변화가 없었다고 신뢰 → log_page 그대로 사용자에게 전달. */
	if (ctx->start_genctr == ctx->end_genctr) {
		/* [한국어] 성공 콜백: rc=0, cpl 전달(상태=성공), log_page에 entry 배열 포함.
		 * 사용자는 numrec 만큼 log_page->entries[]를 순회해 각 NVMe-oF subsystem에
		 * spdk_nvme_connect 호출 가능. log_page 소유권은 사용자에게 이양 — 사용자가 free 책임. */
		ctx->cb_fn(ctx->cb_arg, 0, cpl, ctx->log_page);
	} else {
		/* [한국어] 변화 감지: 받은 log_page가 일관되지 않을 수 있음 → 폐기 후 처음부터 재시작.
		 * realloc로 늘렸을 수 있는 buffer 도 모두 free. */
		free(ctx->log_page);
		/* [한국어] 외부 진입점을 그대로 재호출 — 새 ctx + 새 buffer 할당부터 다시.
		 * 본 함수의 ctx는 곧 free 되므로 cb_fn/cb_arg만 살려서 넘기면 됨. */
		rc = spdk_nvme_ctrlr_get_discovery_log_page(ctx->ctrlr, ctx->cb_fn, ctx->cb_arg);
		if (rc != 0) {
			/* [한국어] 재시작 자체가 동기 실패(ENOMEM 등) → 사용자에게 rc로 보고,
			 * cpl=NULL, log_page=NULL. 재시도 불가능 상황. */
			ctx->cb_fn(ctx->cb_arg, rc, NULL, NULL);
		}
	}
	/* [한국어] 본 ctx는 더 이상 사용되지 않음 — 성공/에러/재시작 모든 경로에서 free.
	 * 재시작 경로는 새 ctx가 위 spdk_nvme_ctrlr_get_discovery_log_page에서 calloc 됐으므로 안전. */
	free(ctx);
}

/*
 * [한국어]
 * get_log_page_completion - 2차(전체 log page) fetch 완료 콜백.
 *
 * @cb_arg: nvme_discovery_ctx 포인터.
 * @cpl: 2차 admin 명령 (전체 log page) 결과 CQE.
 *
 * 이 콜백 시점에 ctx->log_page에는 헤더 + entry 배열이 모두 채워진 "후보 snapshot"이
 * 들어 있다. 그러나 fetch 도중 컨트롤러가 토폴로지를 갱신했을 수 있으므로 본 함수는
 * "snapshot 이후 genctr만 다시 한 번 가져와 비교"하는 3차 admin 명령을 큐잉한다.
 *
 *   - error CQE → log_page 폐기, 사용자 cb_fn(0, cpl, NULL), ctx free.
 *   - 정상 → end_genctr 만 8바이트로 받아오는 추가 Get Log Page 명령 발사 →
 *     완료 시 get_log_page_completion_final 호출.
 *
 * 흥미로운 디테일: 3차 명령은 동일 LID(0x70 Discovery)의 첫 8바이트(즉 genctr)만 읽기
 * 위해 buffer 크기를 sizeof(uint64_t) 로 지정. NVMe Get Log Page는 numd로 길이 인코딩,
 * lba 오프셋 0으로 시작 → 디바이스가 헤더 시작부터 8바이트 회신 → struct 첫 필드인
 * genctr가 그 자리에 채워짐.
 *
 * 호출 체인:
 *   admin q completion poller → [이 함수] → spdk_nvme_ctrlr_cmd_get_log_page
 *     → 3차 admin 발사 → CQE → get_log_page_completion_final
 */
static void
get_log_page_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_discovery_ctx *ctx = cb_arg; /* [한국어] cb_arg → ctx 캐스팅. */
	int rc; /* [한국어] 3차 admin 명령의 동기 반환값. */

	/* [한국어] CQE 에러 검사 — 전체 log page fetch 중 transport/protocol 에러 발생.
	 * 이 경우 즉시 사용자에게 보고하고 정리. */
	if (spdk_nvme_cpl_is_error(cpl)) {
		free(ctx->log_page); /* [한국어] 부분적으로 채워진 log page buffer 폐기. */
		ctx->cb_fn(ctx->cb_arg, 0, cpl, NULL); /* [한국어] rc=0 + cpl 전달(에러 정보). */
		free(ctx); /* [한국어] 컨텍스트 해제. */
		return;
	}

	/* [한국어] 3차 admin 명령: 같은 Discovery log page를 8바이트만 다시 읽어 end_genctr에 저장.
	 * 인자 의미:
	 *   - SPDK_NVME_LOG_DISCOVERY (0x70): NVMe-oF Discovery Log Page identifier
	 *   - nsid=0: 컨트롤러 전역 log (네임스페이스 무관)
	 *   - &ctx->end_genctr: 결과 buffer (8 byte)
	 *   - sizeof(uint64_t) = 8: buffer 크기
	 *   - lba_offset=0: page 시작부터 (genctr가 첫 필드)
	 *   - 콜백: get_log_page_completion_final */
	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctx->ctrlr, SPDK_NVME_LOG_DISCOVERY, 0,
					      &ctx->end_genctr, sizeof(ctx->end_genctr), 0,
					      get_log_page_completion_final, ctx);
	if (rc != 0) {
		/* [한국어] 동기 에러(보통 -ENOMEM: req 풀 또는 admin q 슬롯 고갈).
		 * 이 시점 log_page는 일관성 검증 못 했으므로 폐기 후 사용자에게 rc 보고. */
		free(ctx->log_page);
		ctx->cb_fn(ctx->cb_arg, rc, NULL, NULL);
		free(ctx);
	}
	/* [한국어] rc==0인 경우 ctx는 살려둠 — get_log_page_completion_final이 free 책임. */
}

/*
 * [한국어]
 * discovery_log_header_completion - 1차(헤더만) fetch 완료 콜백.
 *
 * @cb_arg: nvme_discovery_ctx 포인터.
 * @cpl: 1차 admin 명령 (헤더 sizeof(spdk_nvmf_discovery_log_page) 분량) 결과 CQE.
 *
 * 진입점에서 헤더 1개분을 받았으므로 numrec 필드를 읽어 entry 개수를 알 수 있다.
 * 이 함수의 일:
 *   1) error CQE → 사용자에게 보고하고 종료. 이 컨트롤러가 Discovery 컨트롤러가 아닐 수도
 *      있으므로(예: 일반 NVM 컨트롤러에 잘못된 LID로 보냈을 때) 에러 로그 없이 조용히 반환.
 *   2) recfmt(record format) 검증 — 0(현재 spec 정의 유일 값)이 아니면 unsupported 에러.
 *   3) start_genctr 캐싱 (atomic snapshot 비교용 기준).
 *   4) numrec 디코딩 (from_le64 — NVMe spec은 little-endian).
 *   5) numrec == 0 → entry 없으니 헤더만 사용자에게 전달 (get_log_page_completion으로 fall-through).
 *   6) numrec > 0 → realloc으로 buffer 확장 → 전체 log page를 다시 fetch
 *      (next 콜백 = get_log_page_completion).
 *
 * 호출 체인:
 *   admin q completion poller → [이 함수] → spdk_nvme_ctrlr_cmd_get_log_page
 *     → 2차 admin (전체 page) → CQE → get_log_page_completion
 */
static void
discovery_log_header_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvmf_discovery_log_page *new_page; /* [한국어] realloc 후 새 buffer 포인터. */
	struct nvme_discovery_ctx *ctx = cb_arg; /* [한국어] 운반된 컨텍스트. */
	size_t page_size; /* [한국어] 전체 log page 크기 = header + numrec * entry. */
	uint16_t recfmt; /* [한국어] Record Format — spec 0만 정의됨. */
	uint64_t numrec; /* [한국어] Discovery log entry 개수. */
	int rc; /* [한국어] 2차 admin 명령의 동기 반환값. */

	if (spdk_nvme_cpl_is_error(cpl)) {
		/* Return without printing anything - this may not be a discovery controller */
		/* [한국어] 침묵 반환 — 일반 NVM 컨트롤러에서 LID 0x70 시도 시 자연스러운 에러일 수 있음.
		 * 사용자에게 에러 cpl을 그대로 전달하고 정리. */
		ctx->cb_fn(ctx->cb_arg, 0, cpl, NULL);
		free(ctx->log_page); /* [한국어] 1차 fetch 시 calloc 한 buffer 해제. */
		free(ctx);
		return;
	}

	/* Got the first 4K of the discovery log page */
	/* [한국어] recfmt 디코딩 — log page 헤더의 record format 식별자. spec 1.x/2.x 모두 0만 정의.
	 * from_le16: NVMe LE → host endian 변환 (host가 LE면 noop). */
	recfmt = from_le16(&ctx->log_page->recfmt);
	if (recfmt != 0) {
		/* [한국어] 미지원 포맷 — 컨트롤러가 향후 spec extension 사용 가능성. 현재 SPDK는 거부. */
		NVME_CTRLR_ERRLOG(ctx->ctrlr, "Unrecognized discovery log record format %" PRIu16 "\n", recfmt);
		ctx->cb_fn(ctx->cb_arg, -EINVAL, NULL, NULL); /* [한국어] -EINVAL 동기 에러 보고. */
		free(ctx->log_page);
		free(ctx);
		return;
	}

	/* [한국어] start_genctr 저장 — 헤더는 host endian 호환 위치 멤버 접근이지만,
	 * 본 코드는 그냥 직접 대입함 (LE host 전제 또는 spec field native 가정).
	 * Atomic snapshot 비교 시 end_genctr와 같은 방식으로 받아야 의미 있음. */
	ctx->start_genctr = ctx->log_page->genctr;

	/* [한국어] numrec 디코딩 — Discovery entry 개수. spec상 LE 64bit. */
	numrec = from_le64(&ctx->log_page->numrec);

	if (numrec == 0) {
		/* No entries in the discovery log. So we can just return the header to the
		 * caller.
		 */
		/* [한국어] entry 0개 — buffer 확장 불필요, atomic 검증만 마치고 끝내면 됨.
		 * get_log_page_completion으로 직접 점프하면 거기서 end_genctr 재조회 명령을 발사. */
		get_log_page_completion(ctx, cpl);
		return;
	}

	/*
	 * Now that we know how many entries should be in the log page, we can allocate
	 * the full log page buffer.
	 */
	/* [한국어] 전체 page 크기 = (header) + numrec × (entry 64+ byte). header는 entries[0]
	 * 직전까지의 고정 크기. */
	page_size = sizeof(struct spdk_nvmf_discovery_log_page);
	page_size += numrec * sizeof(struct spdk_nvmf_discovery_log_page_entry);
	/* [한국어] realloc — 기존 헤더 데이터(특히 start_genctr 위치 등)는 보존 + 뒤쪽이 확장.
	 * numrec 매우 크면(악의적 컨트롤러) 메모리 폭발 가능 — 실패하면 -ENOMEM 보고. */
	new_page = realloc(ctx->log_page, page_size);
	if (new_page == NULL) {
		NVME_CTRLR_ERRLOG(ctx->ctrlr, "Could not allocate buffer for log page (%" PRIu64 " entries)\n",
				  numrec);
		ctx->cb_fn(ctx->cb_arg, -ENOMEM, NULL, NULL); /* [한국어] -ENOMEM 동기 보고. */
		free(ctx->log_page); /* [한국어] realloc 실패 시 원본은 그대로 유효하므로 free 필요. */
		free(ctx);
		return;
	}

	/* [한국어] realloc 성공 — ctx에 새 포인터 갱신. 옛 ctx->log_page 는 realloc 가 자동 free. */
	ctx->log_page = new_page;

	/* Retrieve the entire discovery log page */
	/* [한국어] 2차 admin: 전체 page를 처음부터 다시 fetch. 첫 fetch와 같은 LID지만 buffer가
	 * 확장됐으므로 entry 배열까지 전부 채워질 것. lba_offset=0 으로 시작. */
	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctx->ctrlr, SPDK_NVME_LOG_DISCOVERY,
					      0, (char *)ctx->log_page, page_size, 0,
					      get_log_page_completion, ctx);
	if (rc != 0) {
		/* [한국어] 2차 명령 큐잉 동기 실패 — 정리 후 사용자에게 rc 보고. */
		free(ctx->log_page);
		ctx->cb_fn(ctx->cb_arg, rc, NULL, NULL);
		free(ctx);
	}
	/* [한국어] rc==0이면 ctx 보존 — get_log_page_completion 또는 그 다음 단계가 free 책임. */
}

/*
 * [한국어]
 * spdk_nvme_ctrlr_get_discovery_log_page - NVMe-oF Discovery Log Page 비동기 조회 진입점.
 *
 * @ctrlr: NVMe-oF Discovery 컨트롤러 핸들 (사용자가 사전에 spdk_nvme_connect로 attach).
 * @cb_fn: 완료 콜백. 시그니처: void cb(void *cb_arg, int rc, const struct spdk_nvme_cpl *cpl,
 *         struct spdk_nvmf_discovery_log_page *log_page).
 *         - rc=0 + log_page!=NULL: 성공, log_page 소유권 이양 (사용자가 free 책임).
 *         - rc=0 + cpl!=NULL + log_page=NULL: NVMe 비동기 에러 (cpl에 SC/SCT 정보).
 *         - rc<0: 동기 시스템 에러 (-ENOMEM, -EINVAL 등). cpl 의미 없음.
 * @cb_arg: cb_fn에 전달할 임의 컨텍스트.
 * @return: 0 성공(요청 큐잉됨, 완료는 cb_fn로), 음수 동기 에러 (이 경우 cb_fn 호출 안 됨).
 *
 * 본 함수는 비동기 콜백 체인을 시작한다:
 *   1) ctx 할당 + 헤더 1개분 buffer 할당
 *   2) ctrlr->cdata_zns 영향 없는 일반 admin Get Log Page 명령 발사 (LID=0x70)
 *   3) 완료 시 discovery_log_header_completion → numrec에 따라 buffer realloc + 전체 fetch
 *   4) → get_log_page_completion → genctr만 다시 fetch
 *   5) → get_log_page_completion_final → genctr 비교 → 사용자 cb_fn 호출 또는 재시작
 *
 * 사용자 책임:
 *   - cb_fn에서 받은 log_page는 free 책임.
 *   - cb_fn이 호출되는 스레드는 admin q owner — cb_fn 안에서 spdk_nvme_connect 등을
 *     동기 호출하면 같은 admin q를 점유 중일 수 있으니 주의.
 *
 * 호출 체인:
 *   사용자 (bdev_nvme/app/nvmf 자동 attach 등)
 *     → [이 함수] → spdk_nvme_ctrlr_cmd_get_log_page
 *       → 1차 admin → CQE → discovery_log_header_completion (이후 콜백 체인)
 */
int
spdk_nvme_ctrlr_get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr,
				       spdk_nvme_discovery_cb cb_fn, void *cb_arg)
{
	struct nvme_discovery_ctx *ctx; /* [한국어] 콜백 체인 운반용 컨텍스트. calloc로 0 초기화. */
	int rc; /* [한국어] 1차 admin 명령의 동기 반환값. */

	/* [한국어] 컨텍스트 할당. calloc → 모든 포인터/카운터 0 초기화 (특히 start/end_genctr). */
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM; /* [한국어] 메모리 부족 — 호출자가 retry 또는 회복 결정. */
	}

	/* [한국어] 1차 fetch 용 buffer — 헤더 1개분(약 1KB). numrec를 알기 전이라 작게 시작.
	 * 헤더 안에 entry[0] 까지 포함 가능하지만 이후 realloc로 확장됨. */
	ctx->log_page = calloc(1, sizeof(*ctx->log_page));
	if (ctx->log_page == NULL) {
		free(ctx); /* [한국어] ctx만 free — log_page는 NULL이라 free 불필요. */
		return -ENOMEM;
	}

	/* [한국어] ctx 초기 필드 설정. */
	ctx->ctrlr = ctrlr;   /* [한국어] 모든 콜백이 추가 admin 명령을 보낼 컨트롤러. */
	ctx->cb_fn = cb_fn;   /* [한국어] 최종 사용자 콜백 — 끝까지 운반. */
	ctx->cb_arg = cb_arg; /* [한국어] 사용자 컨텍스트 — 끝까지 운반. */

	/* [한국어] 1차 admin Get Log Page 발사:
	 *   LID=SPDK_NVME_LOG_DISCOVERY(0x70), nsid=0(컨트롤러 전역),
	 *   buffer=ctx->log_page (sizeof = 헤더 1개분), lba_offset=0,
	 *   콜백=discovery_log_header_completion, cb_arg=ctx.
	 * 호출 시점: 사용자 컨텍스트(보통 메인 init 스레드 또는 bdev attach 스레드).
	 * 큐잉 후 즉시 반환 — 완료는 비동기. */
	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_DISCOVERY, 0,
					      ctx->log_page, sizeof(*ctx->log_page), 0,
					      discovery_log_header_completion, ctx);
	if (rc != 0) {
		/* [한국어] 1차 명령 큐잉 자체가 동기 실패 — req 풀 고갈 또는 admin q 슬롯 부족.
		 * 콜백이 절대 호출되지 않으므로 본 함수가 직접 정리. */
		free(ctx->log_page);
		free(ctx);
	}

	/* [한국어] 사용자에게 동기 결과 반환 (0 또는 음수 errno).
	 * rc==0이면 ctx/log_page는 콜백 체인이 끝까지 운반/해제. */
	return rc;
}
