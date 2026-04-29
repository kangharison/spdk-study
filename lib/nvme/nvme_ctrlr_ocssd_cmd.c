/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 */

/*
 * [한국어 설명] Open-Channel SSD(OCSSD) Admin 커맨드 빌더 (nvme_ctrlr_ocssd_cmd.c)
 *
 * === 파일의 역할 ===
 * 표준 NVMe 스펙 외 **Open-Channel SSD 1.2/2.0 스펙**이 정의하는 vendor-specific
 * admin 명령을 SPDK NVMe 드라이버가 발행할 수 있도록 빌더/디스패처를 제공한다.
 * OCSSD는 호스트가 SSD의 NAND geometry(채널/PU/청크/섹터 수)를 직접 인지하고
 * FTL(Flash Translation Layer)·GC(Garbage Collection)를 호스트 측에서 관리하는
 * "host-managed" 모델이며, 그를 위해 표준 NVMe에는 없는 GEOMETRY admin 명령
 * (opcode 0xE2)이 정의되어 있다. 이 파일은 다음 두 가지를 제공한다:
 *   1) spdk_nvme_ctrlr_is_ocssd_supported(): 컨트롤러가 OCSSD인지 휴리스틱 판정
 *   2) spdk_nvme_ocssd_ctrlr_cmd_geometry(): GEOMETRY admin cmd 비동기 발행
 * 즉 SPDK가 OCSSD를 attach 단계에서 식별하고 geometry 메타데이터를 가져오는
 * 진입점 역할을 한다. 표준 nvme_ctrlr_cmd.c의 admin 빌더 패턴(락→풀 alloc→
 * SQE 채움→submit→락 해제)을 그대로 따르되, OCSSD 전용 opcode와 검증 규칙만
 * 다르다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (geometry):
 *   [bdev_ocssd / lightnvm 사용자] spdk_nvme_ocssd_ctrlr_cmd_geometry()
 *     → nvme_allocate_request_user_copy(adminq, payload, ...) [요청 풀 alloc + bounce 버퍼]
 *     → cmd->opc = SPDK_OCSSD_OPC_GEOMETRY (0xE2)
 *     → nvme_ctrlr_submit_admin_request() → admin qpair tail 기록
 *       → nvme_pcie_qpair_submit_request → MMIO doorbell ring → 디바이스
 *     → 추후 spdk_nvme_ctrlr_process_admin_completions() 폴링 시 cb_fn 호출
 *
 * is_ocssd_supported는 보통 컨트롤러 attach 직후 호출되며, vendor ID 및
 * namespace의 vendor_specific 첫 바이트를 검사하여 OCSSD 여부를 판정한다.
 * 표준화된 식별자가 없으므로 휴리스틱 — QEMU OpenChannel 디바이스(CNEX Labs vid)
 * 한정 분기이다.
 *
 * 실행 컨텍스트: 호출 스레드(보통 ctrlr를 attach한 스레드). nvme_ctrlr_lock으로
 * ctrlr-wide 직렬화. admin qpair는 단일 큐이므로 다수 스레드의 동시 admin 발행을
 * 락으로 보호. 완료 콜백은 admin completions polling 시 동일 스레드에서 호출됨.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/nvme_ocssd.h: OCSSD 공개 API와 자료형(spdk_ocssd_geometry_data,
 *     SPDK_OCSSD_OPC_*, SPDK_PCI_VID_CNEXLABS).
 *   - nvme_internal.h: spdk_nvme_ctrlr/qpair, nvme_request, ctrlr->cdata,
 *     ctrlr->quirks(NVME_QUIRK_OCSSD), ctrlr->adminq, nvme_allocate_request_user_copy,
 *     nvme_ctrlr_submit_admin_request, nvme_ctrlr_lock/unlock,
 *     spdk_nvme_ctrlr_get_first_active_ns/get_ns 선언.
 * 의존하는 측: bdev_ocssd 모듈, OCSSD 예제(test/) 등 — 스펙 deprecated 이후
 *   사용자 코드는 줄어들었으나 인터페이스는 유지.
 * 데이터 흐름:
 *   호출자가 제공한 payload(spdk_ocssd_geometry_data 크기 = 4096B 가정)
 *   → user_copy로 SPDK가 IOVA 가능한 bounce 버퍼에 복사 → admin SQE에 PRP 매핑
 *   → 디바이스가 geometry 데이터를 DMA로 채워 반환
 *   → 완료 시 SPDK가 사용자 buffer로 다시 copy하고 cb_fn 호출
 * 공유 자료구조:
 *   - spdk_nvme_ctrlr: quirks(드라이버가 부여한 디바이스별 워크어라운드 비트마스크),
 *     cdata(IDENTIFY Controller 결과 캐시), adminq.
 *   - spdk_nvme_ns: nsdata.vendor_specific[0] (OCSSD 식별 휴리스틱).
 *
 * === 주요 함수/구조체 요약 ===
 *   - spdk_nvme_ctrlr_is_ocssd_supported(ctrlr) → bool
 *     : QUIRK_OCSSD가 설정되고 (CNEX Labs vid + ns vendor_specific[0]==0x1)인 경우 true.
 *   - spdk_nvme_ocssd_ctrlr_cmd_geometry(ctrlr, nsid, payload, payload_size, cb_fn, cb_arg) → int
 *     : OCSSD GEOMETRY admin 명령(opc 0xE2)을 admin qpair로 비동기 발행.
 *       payload_size는 sizeof(spdk_ocssd_geometry_data) 정확히 일치 필요.
 */

#include "spdk/nvme_ocssd.h"   /* [한국어] OCSSD 공개 API/자료형 — opcode(SPDK_OCSSD_OPC_GEOMETRY), geometry 데이터 구조, vendor ID 매크로 */
#include "nvme_internal.h"     /* [한국어] SPDK NVMe 드라이버 내부 타입·헬퍼 — ctrlr/qpair 구조, request 풀, lock, admin submit 함수 선언 */

/*
 * [한국어]
 * spdk_nvme_ctrlr_is_ocssd_supported - 주어진 컨트롤러가 OCSSD인지 휴리스틱 판정
 *
 * @ctrlr: 검사할 컨트롤러
 * @return: true면 OCSSD로 식별 가능, false면 아님(또는 알 수 없음)
 *
 * 동기/배경: NVMe 표준에는 OCSSD를 식별하는 표준 비트가 없다. 또한 OCSSD
 * 스펙도 LightNVM 1.2 → OCSSD 2.0으로 진화하면서 다양한 vendor가 vendor-specific
 * 방식으로 식별 정보를 노출했다. SPDK는 다음 휴리스틱을 사용한다:
 *   1) 드라이버가 디바이스 quirks 테이블에서 NVME_QUIRK_OCSSD 비트를 부여했는지
 *      (PCI VID/DID 매칭으로 사전 등록된 OCSSD 디바이스).
 *   2) 추가로 CNEX Labs vendor (QEMU OpenChannel 에뮬레이션 vid)인 경우,
 *      첫 active namespace의 IDENTIFY NS 결과에서 vendor_specific[0]==0x1인지.
 * 둘 다 만족하면 OCSSD로 판정. 그렇지 않으면 false.
 *
 * 호출 체인:
 *   [user/bdev_ocssd] → spdk_nvme_ctrlr_is_ocssd_supported
 *     → spdk_nvme_ctrlr_get_first_active_ns / get_ns (ns 메타데이터 조회)
 */
bool
spdk_nvme_ctrlr_is_ocssd_supported(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->quirks & NVME_QUIRK_OCSSD) {
	                                /* [한국어] 1차 조건: 드라이버가 디바이스를 OCSSD 후보로 사전 등록(quirks 테이블)했는가
	                                 *  - quirks는 lib/nvme/nvme_quirks.c의 PCI ID 매칭 결과로 ctrlr 생성 시 채워짐 */
		/* TODO: There isn't a standardized way to identify Open-Channel SSD
		 * different verdors may have different conditions.
		 */
	                                /* [한국어] 표준화된 식별 수단 부재 — 미래에 NVMe Multiple Command Set Identifiers 등으로
	                                 *           대체될 수 있으나, 현재(2024)는 vendor 별 휴리스틱뿐 */

		/*
		 * Current QEMU OpenChannel Device needs to check nsdata->vs[0].
		 * Here check nsdata->vs[0] of the first namespace.
		 */
	                                /* [한국어] QEMU OpenChannel 에뮬레이션은 IDENTIFY NS의 vendor_specific[0]에 0x1 마커
	                                 *           기록 — 첫 번째 active namespace로 충분히 대표 가능 */
		if (ctrlr->cdata.vid == SPDK_PCI_VID_CNEXLABS) {
	                                /* [한국어] 2차 조건: IDENTIFY Controller에 노출된 PCI Vendor ID가 CNEX Labs(QEMU)
	                                 *           - cdata.vid는 IDENTIFY Controller(CNS=1)의 VID 필드(0~1B) 캐시 */
			uint32_t nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	                                /* [한국어] 첫 active namespace ID 조회 — 1부터 시작, 활성 ns가 없으면 0 */
			struct spdk_nvme_ns *ns;
	                                /* [한국어] ns 객체 포인터 — 메타데이터(nsdata) 접근용 */

			if (nsid == 0) {
	                                /* [한국어] active ns가 하나도 없으면 휴리스틱 판정 불가 → OCSSD 아님으로 처리 */
				return false;
			}

			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	                                /* [한국어] nsid → ns 포인터 조회. 실패 시 NULL */

			if (ns && ns->nsdata.vendor_specific[0] == 0x1) {
	                                /* [한국어] ns가 유효하고 IDENTIFY NS의 vendor_specific[0]이 0x1이면 OCSSD 확정
	                                 *  - vendor_specific은 IDENTIFY NS(CNS=0) 응답의 384B vendor 영역
	                                 *  - QEMU OpenChannel: 첫 바이트로 OCSSD 여부 표시 */
				return true;
			}
		}
	}
	return false;                   /* [한국어] 위 모든 조건 불일치 — OCSSD 아님 */
}


/*
 * [한국어]
 * spdk_nvme_ocssd_ctrlr_cmd_geometry - OCSSD GEOMETRY admin cmd 비동기 발행
 *
 * @ctrlr:        대상 OCSSD 컨트롤러 (PCIe)
 * @nsid:         geometry 정보를 조회할 namespace ID (1~)
 * @payload:      디바이스가 geometry 데이터를 채울 호스트 버퍼
 *                (크기 정확히 sizeof(struct spdk_ocssd_geometry_data) 바이트)
 * @payload_size: 위 버퍼 크기 — 검증을 위해 받음
 * @cb_fn:        완료 콜백 — (cb_arg, cqe) 시그니처
 * @cb_arg:       콜백 컨텍스트
 * @return:       0 성공(요청이 admin queue에 enqueue됨, 완료 비동기),
 *                -EINVAL(payload NULL 또는 크기 불일치),
 *                -ENOMEM(요청 풀 고갈)
 *
 * 동기/배경: OCSSD 호스트는 LBA를 NAND 채널/PU/청크/섹터로 매핑하기 위해
 * geometry를 알아야 한다. GEOMETRY는 NVMe Admin opcode 0xE2(OCSSD vendor-specific)
 * 명령으로, 호스트가 PRP 버퍼를 제공하면 컨트롤러가 그 버퍼에 4096B의
 * spdk_ocssd_geometry_data를 채워 반환한다. 이 함수는 SQE를 빌드하여
 * admin qpair에 제출만 하고, 실제 완료는 polling으로 통지된다.
 *
 * 동작:
 *   1) payload 검증 (NULL/크기) → 잘못되면 즉시 -EINVAL
 *   2) ctrlr 락 획득 (admin qpair 직렬화)
 *   3) admin qpair 풀에서 user_copy 형태로 nvme_request 할당
 *      (디바이스는 IOVA 매핑된 SPDK 메모리만 DMA 가능하므로 호스트 buffer를
 *       SPDK 내부 bounce 버퍼로 복사. is_admin=false → write 방향 false)
 *   4) SQE의 opcode를 SPDK_OCSSD_OPC_GEOMETRY(0xE2)로, NSID 설정
 *   5) admin qpair에 submit
 *   6) 락 해제 후 rc 반환
 *
 * 호출 체인:
 *   [bdev_ocssd setup] → spdk_nvme_ocssd_ctrlr_cmd_geometry
 *     → nvme_allocate_request_user_copy → nvme_ctrlr_submit_admin_request
 *     → 트랜스포트(submit_request) → SQ enqueue + doorbell
 *     ... (디바이스 처리) ...
 *     → spdk_nvme_ctrlr_process_admin_completions(polling) → cb_fn(cb_arg, cqe)
 */
int
spdk_nvme_ocssd_ctrlr_cmd_geometry(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				   void *payload, uint32_t payload_size,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;       /* [한국어] admin qpair 요청 풀에서 받아올 요청 객체 — SQE/cb/payload meta 보유 */
	struct spdk_nvme_cmd *cmd;      /* [한국어] req 내부 SQE(64B)에 대한 편의 포인터 — opc/nsid/cdw 채움용 */
	int rc;                         /* [한국어] submit 결과 보관 — 호출자에게 반환 */

	if (!payload || (payload_size != sizeof(struct spdk_ocssd_geometry_data))) {
	                                /* [한국어] payload 검증
	                                 *  - NULL 거부: DMA 대상 버퍼 필수
	                                 *  - 크기 정확히 일치: GEOMETRY 응답이 정확히 4096B여야 디바이스가 PRP 한 페이지에 기록 가능 */
		return -EINVAL;             /* [한국어] 잘못된 인자 — 락 잡기 전 fast-fail */
	}

	nvme_ctrlr_lock(ctrlr);         /* [한국어] ctrlr 락 — admin qpair는 단일 큐이므로 동시 발행 직렬화
	                                 *  - process_init이나 사용자 다른 admin 호출과 경쟁 방지 */
	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, payload_size, cb_fn, cb_arg, false);
	                                /* [한국어] admin qpair 풀에서 요청 alloc + bounce buffer 처리
	                                 *  - 마지막 인자 false = host_to_controller 방향 false → 디바이스→호스트(read)
	                                 *  - SPDK가 IOVA 가능한 내부 버퍼를 잡고, 완료 시 사용자 payload로 결과 copy */
	if (req == NULL) {
	                                /* [한국어] 풀 고갈 — 호출자는 admin completions polling 후 재시도 권장 */
		nvme_ctrlr_unlock(ctrlr);   /* [한국어] 에러 경로에서도 락 해제 누락 금지 */
		return -ENOMEM;             /* [한국어] 자원 부족 errno 반환 */
	}

	cmd = &req->cmd;                /* [한국어] 풀에서 받은 요청의 SQE에 대한 in-place 빌드 진입 */
	cmd->opc = SPDK_OCSSD_OPC_GEOMETRY;
	                                /* [한국어] OCSSD 1.2/2.0 admin opcode 0xE2 — vendor-specific 영역(0xC0~0xFF) */
	cmd->nsid = nsid;               /* [한국어] 대상 namespace — geometry는 ns별로 다를 수 있음 */
	                                /* [한국어] PRP1/PRP2는 nvme_allocate_request_user_copy + 트랜스포트 submit이
	                                 *           bounce buffer 기반으로 자동 채움 — 여기서는 명시 설정 불필요 */

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	                                /* [한국어] admin qpair에 요청 enqueue
	                                 *  - PCIe 트랜스포트면 SQ에 SQE 기록 + tail doorbell 갱신
	                                 *  - 비동기 — 완료는 추후 polling으로 cb_fn 호출 */

	nvme_ctrlr_unlock(ctrlr);       /* [한국어] 정상 경로에서도 락 해제 — submit 이후 디바이스 처리는 락 외부에서 진행 */
	return rc;                      /* [한국어] submit 결과 반환 (0=enqueue 성공, 음수=즉시 에러) */
}
