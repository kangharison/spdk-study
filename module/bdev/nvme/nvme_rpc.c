/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] NVMe Admin/IO 명령 passthrough RPC (nvme_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 "bdev_nvme_send_cmd"라는 단일 JSON-RPC 메서드를 등록하여, 호스트 사용자가
 * 임의의 NVMe Admin 또는 IO 명령(64바이트 SQE)을 base64로 직렬화해서 전송하면, SPDK가
 * 점유 중인 컨트롤러로 실제 발행해 응답(CQE 16바이트 + 선택적 read 데이터/메타데이터)을
 * base64로 돌려준다. 즉 nvme-cli의 `nvme-passthrough` 또는 vendor-unique 명령 발급을
 * SPDK 환경에서도 가능하게 하는 디버깅/관리 도구이다. CUSE(/dev/nvme noeud)를 사용하지
 * 못하는 환경에서 같은 목적을 달성한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   외부 RPC 클라이언트 (scripts/rpc.py bdev_nvme_send_cmd ...)
 *     → SPDK JSON-RPC 서버
 *     → rpc_bdev_nvme_send_cmd() (요청 디코드 → 컨텍스트 할당)
 *     → rpc_bdev_nvme_send_cmd_exec() (cmd_type 분기)
 *         → admin: spdk_nvme_ctrlr_cmd_admin_raw() (lib/nvme)
 *         → io:   spdk_nvme_ctrlr_cmd_io_raw_with_md() (lib/nvme)
 *     → NVMe SQ doorbell write → 디바이스 → CQ → poller가 callback
 *     → nvme_rpc_bdev_nvme_cb()
 *     → rpc_bdev_nvme_send_cmd_complete() (CPL/data/md를 base64로 응답)
 * 실행 컨텍스트: SPDK app 스레드의 RPC 콜백 (cmd 발행), completion 콜백은 admin queue
 * poller(또는 IO qpair poll group poller)에서 실행. 같은 SPDK 스레드라 cross-thread
 * 동기화 불필요.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/rpc.h, spdk/util.h, spdk/string.h, spdk/log.h, spdk/bdev_module.h,
 *         spdk/base64.h (JSON에 바이너리 SQE/데이터 base64 인코딩),
 *         bdev_nvme.h (nvme_ctrlr lookup), spdk/nvme.h (raw cmd 발행 API).
 * - 의존받음: 별도 헤더 없음. SPDK_RPC_REGISTER로 등록.
 * - 데이터 흐름:
 *   클라이언트 JSON {name, cmd_type, data_direction, cmdbuf(base64), data_len, data(base64), …}
 *     → req 구조체 (cmdbuf=64B SQE 사본, data=DMA 가능 버퍼)
 *     → spdk_nvme_*_raw API에 cmdbuf/data/md 포인터 전달
 *     → 디바이스가 PRP/SGL로 data/md 버퍼에 DMA 수행
 *     → CQE 도착 시 nvme_rpc_bdev_nvme_cb 실행 → resp 빌드 → JSON 응답 전송.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rpc_bdev_nvme_send_cmd_ctx: 한 번의 RPC 호출 라이프사이클을 관리하는 컨텍스트
 *   (req+resp+nvme_ctrlr+io_channel 보유, async cb까지 살아있음).
 * - rpc_bdev_nvme_send_cmd: 진입점. JSON 디코드 + 컨트롤러 lookup + exec.
 * - nvme_rpc_admin_cmd_bdev_nvme / nvme_rpc_io_cmd_bdev_nvme: 각 cmd_type 분기.
 * - nvme_rpc_bdev_nvme_cb: completion 콜백, IO 채널 해제 후 응답 빌드.
 * - rpc_decode_*: cmd_type/data_direction/cmdbuf/data/metadata 등 커스텀 디코더.
 *   data/metadata는 두 경로(text 우선/길이 우선)에서 모두 디코딩 가능하도록 분기 처리.
 */

#include "spdk/stdinc.h"        /* [한국어] 표준 C 헤더. */
#include "spdk/string.h"        /* [한국어] spdk_strerror. */
#include "spdk/rpc.h"            /* [한국어] SPDK_RPC_REGISTER, JSON-RPC 응답. */
#include "spdk/util.h"           /* [한국어] SPDK_COUNTOF. */
#include "spdk/bdev_module.h"   /* [한국어] bdev 모듈 헬퍼 (필요한 매크로). */
#include "spdk/log.h"            /* [한국어] SPDK_ERRLOG/SPDK_NOTICELOG. */

#include "bdev_nvme.h"           /* [한국어] nvme_ctrlr, nvme_ctrlr_get_by_name 등 모듈 내부 API. */
#include "spdk/base64.h"         /* [한국어] base64 url-safe 인코딩/디코딩 (binary↔JSON string). */

/*
 * [한국어]
 * enum spdk_nvme_rpc_type - bdev_nvme_send_cmd가 발행할 NVMe 명령의 종류.
 */
enum spdk_nvme_rpc_type {
	NVME_ADMIN_CMD = 1,   /* [한국어] Admin queue로 발행 (Identify, Get Log Page, Format 등). */
	NVME_IO_CMD,          /* [한국어] IO qpair로 발행 (Read/Write/Compare 등 또는 vendor-unique). */
};

/*
 * [한국어]
 * struct rpc_bdev_nvme_send_cmd_req - JSON 입력을 디코딩한 요청 구조체.
 *
 * cmdbuf는 NVMe 64바이트 SQE 사본을 가리키고, data/md는 DMA 가능한 hugepage 버퍼.
 * data_direction에 따라 host→ctrlr이면 클라이언트가 미리 채운 data가 디바이스로,
 * ctrlr→host면 디바이스가 data에 DMA write 후 응답에 base64로 포함.
 */
struct rpc_bdev_nvme_send_cmd_req {
	char			*name;
	/* [한국어] 명령을 발행할 대상 NVMe 컨트롤러 이름 (예: "Nvme0").
	 * 설정자: spdk_json_decode_object()가 spdk_json_decode_string()으로 strdup.
	 * 읽는 자: rpc_bdev_nvme_send_cmd()에서 nvme_ctrlr_get_by_name() 인자.
	 * 값 범위: null-terminated 문자열. 필수. 없으면 디코딩 실패.
	 * 동기화: 단일 RPC 컨텍스트(ctx->req.name). free_rpc_bdev_nvme_send_cmd_ctx()에서 free. */

	int			cmd_type;
	/* [한국어] 발행할 NVMe 명령의 종류: NVME_ADMIN_CMD(1) 또는 NVME_IO_CMD(2).
	 * 설정자: rpc_decode_cmd_type()에서 "admin"→NVME_ADMIN_CMD, "io"→NVME_IO_CMD.
	 * 읽는 자: rpc_bdev_nvme_send_cmd_cb() 내 nvme_rpc_send_cmd()에서 switch로 분기.
	 * 값 범위: NVME_ADMIN_CMD(1) 또는 NVME_IO_CMD(2). 다른 값이면 -EINVAL.
	 * 동기화: 단일 RPC 컨텍스트 내. 변경 없음. */

	int			data_direction;
	/* [한국어] DMA 전송 방향: host→device (h2c) 또는 device→host (c2h).
	 * 설정자: rpc_decode_data_direction()에서 "h2c"→SPDK_NVME_DATA_HOST_TO_CONTROLLER, "c2h"→역방향.
	 * 읽는 자: rpc_bdev_nvme_send_cmd_resp_construct()에서 c2h일 때 data_text를 응답에 포함.
	 * 값 범위: SPDK_NVME_DATA_HOST_TO_CONTROLLER 또는 SPDK_NVME_DATA_CONTROLLER_TO_HOST.
	 * 동기화: 단일 RPC 컨텍스트 내. 변경 없음. */

	uint32_t		timeout_ms;
	/* [한국어] NVMe 명령 타임아웃 (밀리초). 현재 lib/nvme raw cmd API에는 전달되지 않고 예약됨.
	 * 설정자: spdk_json_decode_uint32(). 디코더 테이블에 optional=true로 등록됨.
	 * 읽는 자: 현재 코드에서는 미사용. 향후 spdk_nvme_ctrlr_cmd_admin_raw timeout 확장 시 사용 예정.
	 * 값 범위: 0 이상. 0이면 기본 타임아웃 적용.
	 * 동기화: 단일 RPC 컨텍스트 내. 변경 없음. */

	uint32_t		data_len;
	/* [한국어] DMA 데이터 버퍼 크기 (바이트). data 필드와 연동됨.
	 * 설정자: rpc_decode_data_len() 또는 rpc_decode_data() 내 일관성 체크로 결정.
	 *   - data_len이 먼저 디코딩되면 길이를 기록, data 디코딩 시 일관성 검증.
	 *   - data가 먼저 디코딩되면 base64 길이로 역산하여 채움.
	 * 읽는 자: nvme_send_cmd_io()/admin()에서 데이터 버퍼 크기 인자로 전달.
	 * 값 범위: 0 이상. 0이면 DMA 없이 non-data 명령.
	 * 동기화: 단일 RPC 컨텍스트. completion까지 불변. */

	uint32_t		md_len;
	/* [한국어] DMA metadata 버퍼 크기 (바이트). DIF/DIX(PI, Protection Information) 용도.
	 * 설정자: rpc_decode_metadata_len() 또는 rpc_decode_metadata()에서 결정.
	 * 읽는 자: nvme_send_cmd_io()에서 spdk_nvme_ns_cmd_read_with_md() md 크기 인자.
	 * 값 범위: 0 이상. 0이면 metadata 없음. PI 사용 namespace에서만 의미 있음.
	 * 동기화: 단일 RPC 컨텍스트. completion까지 불변. */

	struct spdk_nvme_cmd	*cmdbuf;
	/* [한국어] 64바이트 NVMe SQE (Submission Queue Entry) 사본 (JSON base64 디코딩 결과).
	 * 설정자: rpc_decode_cmdbuf()에서 base64 디코딩 후 malloc된 버퍼 포인터 저장.
	 * 읽는 자: nvme_send_cmd_admin()/io()에서 lib/nvme raw cmd API의 cmd 인자로 직접 전달.
	 * 값 범위: 정확히 sizeof(struct spdk_nvme_cmd)==64바이트. 다르면 -EINVAL.
	 * 동기화: 한 RPC 컨텍스트에서만 사용. free_rpc_bdev_nvme_send_cmd_ctx()에서 free. */

	char			*data;
	/* [한국어] DMA 가능한 데이터 버퍼 (spdk_malloc(SPDK_MALLOC_DMA|SPDK_MALLOC_SHARE)로 할당).
	 * 설정자: rpc_decode_data()에서 hugepage 버퍼 할당 후 base64 디코딩 결과를 담음.
	 * 읽는 자: nvme_send_cmd_admin()/io()에서 payload 인자로 전달. c2h면 응답에 base64 포함.
	 * 값 범위: data_len 바이트의 hugepage 버퍼. 4KB 정렬 보장. NULL이면 non-data 명령.
	 * 동기화: completion 콜백까지 살아있어야 함. spdk_free()로만 해제. */

	char			*md;
	/* [한국어] DMA 가능한 metadata 버퍼 (PI/DIF용; spdk_malloc(SPDK_MALLOC_DMA)로 할당).
	 * 설정자: rpc_decode_metadata()에서 hugepage 버퍼 할당 후 base64 디코딩.
	 * 읽는 자: nvme_send_cmd_io()에서 spdk_nvme_ns_cmd_*_with_md의 md 인자로 전달.
	 * 값 범위: md_len 바이트의 hugepage 버퍼. NULL이면 metadata 없음.
	 * 동기화: completion까지 살아있어야 함. spdk_free()로만 해제. */
};

/*
 * [한국어]
 * struct rpc_bdev_nvme_send_cmd_resp - 응답 빌드용 구조체. 모두 base64 인코딩된 텍스트.
 */
struct rpc_bdev_nvme_send_cmd_resp {
	char	*cpl_text;
	/* [한국어] NVMe CPL(Completion Queue Entry) 16바이트를 base64 url-safe 인코딩한 문자열.
	 * 설정자: rpc_bdev_nvme_send_cmd_resp_construct()에서 spdk_base64_urlsafe_encode()로 생성.
	 * 읽는 자: rpc_bdev_nvme_send_cmd_cb()에서 JSON 응답 객체의 "cpl" 필드로 write.
	 * 값 범위: 항상 non-NULL (성공 경로). CPL에는 SC(Status Code)/SCT 등 완료 정보 포함.
	 * 동기화: completion 콜백 내에서만 생성/참조. free_rpc_bdev_nvme_send_cmd_ctx()에서 free. */

	char	*data_text;
	/* [한국어] c2h(device→host) 명령에서 디바이스가 DMA 쓴 데이터를 base64 인코딩한 문자열.
	 * 설정자: rpc_bdev_nvme_send_cmd_resp_construct()에서 data_direction==c2h일 때만 생성.
	 * 읽는 자: rpc_bdev_nvme_send_cmd_cb()에서 data_text가 non-NULL이면 "data" 필드로 write.
	 * 값 범위: c2h 명령에서만 non-NULL. h2c나 non-data 명령에서는 NULL.
	 * 동기화: completion 콜백 내에서만 생성/참조. free_rpc_bdev_nvme_send_cmd_ctx()에서 free. */

	char	*md_text;
	/* [한국어] c2h 명령에서 metadata도 있을 때(PI 포함) DMA 결과를 base64 인코딩한 문자열.
	 * 설정자: rpc_bdev_nvme_send_cmd_resp_construct()에서 c2h이고 md != NULL일 때 생성.
	 * 읽는 자: rpc_bdev_nvme_send_cmd_cb()에서 md_text가 non-NULL이면 "metadata" 필드로 write.
	 * 값 범위: c2h이고 metadata 있는 경우에만 non-NULL. 나머지는 NULL.
	 * 동기화: completion 콜백 내에서만 생성/참조. free_rpc_bdev_nvme_send_cmd_ctx()에서 free. */
};

/*
 * [한국어]
 * struct rpc_bdev_nvme_send_cmd_ctx - RPC 호출 라이프사이클 전체 컨텍스트.
 *
 * 비동기 cmd 발행 후 completion까지 유지되어야 하므로 calloc으로 힙에 할당.
 * cb에서 free_rpc_bdev_nvme_send_cmd_ctx()로 해제.
 */
struct rpc_bdev_nvme_send_cmd_ctx {
	struct spdk_jsonrpc_request	*jsonrpc_request;
	/* [한국어] 응답을 전송할 SPDK JSON-RPC 서버 요청 핸들.
	 * 설정자: rpc_bdev_nvme_send_cmd()에서 request 파라미터를 저장.
	 * 읽는 자: rpc_bdev_nvme_send_cmd_cb()에서 spdk_jsonrpc_begin_result() 인자로 사용.
	 * 값 범위: 유효한 포인터 (NULL 불가). 응답 전송 후 서버 측에서 무효화됨.
	 * 동기화: 비동기 완료까지 단일 RPC 컨텍스트. 응답 후에는 접근 금지. */

	struct rpc_bdev_nvme_send_cmd_req	req;
	/* [한국어] JSON 파라미터 디코딩 결과를 담는 요청 구조체 (name, cmdbuf, data 등 포함).
	 * 설정자: rpc_bdev_nvme_send_cmd()에서 디코더로 채움.
	 * 읽는 자: nvme_rpc_send_cmd()에서 cmd_type 스위치, nvme_send_cmd_*()에서 각 필드 사용.
	 * 값 범위: 디코딩 성공 후 유효. 실패 시 ctx 자체를 free.
	 * 동기화: 비동기 완료까지 ctx와 동일 수명. free_rpc_bdev_nvme_send_cmd_ctx()에서 해제. */

	struct rpc_bdev_nvme_send_cmd_resp	resp;
	/* [한국어] completion 시 JSON 응답에 포함될 base64 인코딩 문자열들을 보관하는 구조체.
	 * 설정자: rpc_bdev_nvme_send_cmd_resp_construct()에서 base64 인코딩 후 포인터 저장.
	 * 읽는 자: rpc_bdev_nvme_send_cmd_cb()에서 JSON 응답 빌드 시 각 필드 write.
	 * 값 범위: cpl_text는 항상 non-NULL(성공), data_text/md_text는 c2h+존재 시만 non-NULL.
	 * 동기화: completion 콜백 내 단일 스레드. free_rpc_bdev_nvme_send_cmd_ctx()에서 각 필드 free. */

	struct nvme_ctrlr		*nvme_ctrlr;
	/* [한국어] 명령을 발행할 대상 NVMe 컨트롤러 객체 포인터.
	 * 설정자: rpc_bdev_nvme_send_cmd()에서 nvme_ctrlr_get_by_name()으로 lookup 후 저장.
	 * 읽는 자: nvme_send_cmd_admin()에서 spdk_nvme_ctrlr_cmd_admin_raw()의 첫 인자.
	 * 값 범위: 유효한 포인터 (lookup 실패 시 NULL, 이 경우 에러 응답 후 ctx free).
	 * 동기화: RPC 컨텍스트 내. 컨트롤러는 completion 이전에 제거되면 안 됨. */

	struct spdk_io_channel		*ctrlr_io_ch;
	/* [한국어] IO 명령 발행 시 획득하는 nvme_ctrlr IO 채널 (NVMe qpair를 포함).
	 * 설정자: nvme_send_cmd_io()에서 spdk_get_io_channel(nvme_ctrlr)로 획득.
	 * 읽는 자: nvme_send_cmd_io()에서 bdev_nvme_get_io_qpair()로 qpair 추출; completion 후 put.
	 * 값 범위: IO 명령에서만 non-NULL. Admin 명령에서는 NULL (admin queue 사용).
	 * 동기화: completion 콜백에서 spdk_put_io_channel()로 반드시 해제해야 qpair 점유가 풀림. */
};

/*
 * [한국어]
 * free_rpc_bdev_nvme_send_cmd_ctx - 컨텍스트와 그 안의 모든 동적 메모리 일괄 해제.
 *
 * @ctx: 해제 대상 컨텍스트.
 *
 * data/md는 spdk_malloc(SPDK_MALLOC_DMA)로 할당된 hugepage이므로 spdk_free 사용,
 * 나머지 strdup/malloc 결과는 일반 free.
 * 호출자: completion 콜백 또는 invalid 분기 (RPC 에러 응답 후).
 */
static void
free_rpc_bdev_nvme_send_cmd_ctx(struct rpc_bdev_nvme_send_cmd_ctx *ctx)
{
	assert(ctx != NULL);   /* [한국어] NULL 컨텍스트는 호출자 버그. */

	free(ctx->req.name);          /* [한국어] strdup된 컨트롤러 이름. */
	free(ctx->req.cmdbuf);        /* [한국어] base64 디코딩 결과(SQE 사본). 일반 malloc. */
	spdk_free(ctx->req.data);     /* [한국어] DMA 버퍼는 반드시 spdk_free. */
	spdk_free(ctx->req.md);       /* [한국어] DMA 메타데이터 버퍼. */
	free(ctx->resp.cpl_text);     /* [한국어] base64 응답 텍스트. */
	free(ctx->resp.data_text);
	free(ctx->resp.md_text);
	free(ctx);                     /* [한국어] 컨텍스트 자체. */
}

/*
 * [한국어]
 * rpc_bdev_nvme_send_cmd_resp_construct - CPL과 (c2h일 때) data/metadata를 base64로 인코딩.
 *
 * @resp: 출력. 각 텍스트 포인터를 malloc + 인코딩으로 채움.
 * @req:  data_direction/data_len/md_len 참조용.
 * @cpl:  16바이트 NVMe Completion Queue Entry.
 * @return: 0 성공, -ENOMEM 메모리 부족.
 *
 * cpl_text는 항상 채워지고, data_text/md_text는 c2h(controller→host)일 때만 채워진다.
 * h2c(host→controller)는 응답에 데이터가 없으므로 cpl만 보낸다.
 */
static int
rpc_bdev_nvme_send_cmd_resp_construct(struct rpc_bdev_nvme_send_cmd_resp *resp,
				      struct rpc_bdev_nvme_send_cmd_req *req,
				      const struct spdk_nvme_cpl *cpl)
{
	/* [한국어] CPL 16바이트를 base64 url-safe로 인코딩 (+1은 NUL 종결). */
	resp->cpl_text = malloc(spdk_base64_get_encoded_strlen(sizeof(*cpl)) + 1);
	if (!resp->cpl_text) {
		return -ENOMEM;
	}
	spdk_base64_urlsafe_encode(resp->cpl_text, cpl, sizeof(*cpl));

	if (req->data_direction == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* [한국어] c2h 분기: 디바이스가 host 메모리(data 버퍼)에 DMA write 했으므로 응답 포함. */
		if (req->data_len) {
			resp->data_text = malloc(spdk_base64_get_encoded_strlen(req->data_len) + 1);
			if (!resp->data_text) {
				return -ENOMEM;
			}
			spdk_base64_urlsafe_encode(resp->data_text, req->data, req->data_len);
		}
		if (req->md_len) {
			resp->md_text = malloc(spdk_base64_get_encoded_strlen(req->md_len) + 1);
			if (!resp->md_text) {
				return -ENOMEM;
			}
			spdk_base64_urlsafe_encode(resp->md_text, req->md, req->md_len);
		}
	}

	return 0;
}

/*
 * [한국어]
 * rpc_bdev_nvme_send_cmd_complete - completion 콜백 본체. JSON 응답 빌드 후 ctx 해제.
 *
 * @ctx: 진행 중인 RPC 호출 컨텍스트.
 * @cpl: 디바이스가 반환한 NVMe CPL.
 *
 * resp 필드를 base64로 채우고, JSON 객체 {cpl, data?, metadata?}로 직렬화 후 RPC 응답 전송.
 * 어느 단계에서든 실패하면 INTERNAL_ERROR 응답. 항상 free_*ctx() 호출 (성공/실패 모두).
 * 실행 컨텍스트: lib/nvme의 admin/io completion 콜백 (보통 같은 SPDK 스레드).
 */
static void
rpc_bdev_nvme_send_cmd_complete(struct rpc_bdev_nvme_send_cmd_ctx *ctx,
				const struct spdk_nvme_cpl *cpl)
{
	struct spdk_jsonrpc_request *request = ctx->jsonrpc_request;   /* [한국어] 응답 대상 RPC. */
	struct spdk_json_write_ctx *w;                                   /* [한국어] 응답 JSON writer. */
	int ret;                                                          /* [한국어] resp 빌드 결과. */

	/* [한국어] 1단계: base64 인코딩으로 resp 구조체 채우기. */
	ret = rpc_bdev_nvme_send_cmd_resp_construct(&ctx->resp, &ctx->req, cpl);
	if (ret) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-ret));
		goto out;
	}

	/* [한국어] 2단계: JSON 응답 빌드. result는 객체. */
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	/* [한국어] 항상 cpl 필드. NVMe CPL의 phase/SC/SCT 등은 클라이언트가 디코딩해서 본다. */
	spdk_json_write_named_string(w, "cpl", ctx->resp.cpl_text);

	if (ctx->resp.data_text) {
		/* [한국어] c2h read 데이터 (있을 때만). */
		spdk_json_write_named_string(w, "data", ctx->resp.data_text);
	}

	if (ctx->resp.md_text) {
		/* [한국어] c2h metadata (있을 때만). */
		spdk_json_write_named_string(w, "metadata", ctx->resp.md_text);
	}

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

out:
	/* [한국어] 항상 컨텍스트 해제. completion 후에는 더 이상 참조 없음. */
	free_rpc_bdev_nvme_send_cmd_ctx(ctx);
	return;
}

/*
 * [한국어]
 * nvme_rpc_bdev_nvme_cb - lib/nvme에 등록된 raw cmd completion 콜백.
 *
 * @ref: spdk_nvme_ctrlr_cmd_*_raw에 cb_arg로 전달했던 ctx 포인터 (void *로 캐스팅).
 * @cpl: 16바이트 NVMe Completion (NVMe spec 4.6 참조).
 *
 * IO 명령이었다면 잡아두었던 ctrlr_io_ch를 spdk_put_io_channel로 해제 (channel ref--).
 * 이후 _complete()로 응답 빌드를 위임.
 * 실행 컨텍스트: admin queue poller (admin) 또는 io qpair poll group poller (io).
 *               같은 SPDK 스레드 안에서 RPC 핸들러가 호출했기 때문에 cross-thread 처리 불필요.
 */
static void
nvme_rpc_bdev_nvme_cb(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct rpc_bdev_nvme_send_cmd_ctx *ctx = (struct rpc_bdev_nvme_send_cmd_ctx *)ref;

	if (ctx->ctrlr_io_ch) {
		/* [한국어] IO 명령이었으면 채널 참조를 해제 (qpair 점유 해제). */
		spdk_put_io_channel(ctx->ctrlr_io_ch);
		ctx->ctrlr_io_ch = NULL;
	}

	rpc_bdev_nvme_send_cmd_complete(ctx, cpl);
}

/*
 * [한국어]
 * nvme_rpc_admin_cmd_bdev_nvme - Admin queue로 raw NVMe 명령 발행.
 *
 * @ctx: RPC 컨텍스트 (cb_arg로 전달).
 * @cmd: 64바이트 SQE 사본.
 * @buf: data 버퍼 (nbytes만큼 PRP/SGL 대상).
 * @nbytes: 데이터 길이.
 * @timeout_ms: 사용 안 함 (lib/nvme에서 별도 timeout 관리).
 * @return: 0 성공, 음수 errno (큐 full 등).
 *
 * spdk_nvme_ctrlr_cmd_admin_raw는 컨트롤러의 Admin SQ에 cmd를 enqueue하고 doorbell write.
 * completion은 admin queue poller가 처리하며 nvme_rpc_bdev_nvme_cb 호출.
 */
static int
nvme_rpc_admin_cmd_bdev_nvme(struct rpc_bdev_nvme_send_cmd_ctx *ctx, struct spdk_nvme_cmd *cmd,
			     void *buf, uint32_t nbytes, uint32_t timeout_ms)
{
	struct nvme_ctrlr *_nvme_ctrlr = ctx->nvme_ctrlr;   /* [한국어] 대상 컨트롤러. */
	int ret;                                             /* [한국어] 발행 결과. */

	/* [한국어] lib/nvme의 admin raw API. lib/nvme이 cmd 사본을 만들고 ASQ에 enqueue 후 doorbell. */
	ret = spdk_nvme_ctrlr_cmd_admin_raw(_nvme_ctrlr->ctrlr, cmd, buf,
					    nbytes, nvme_rpc_bdev_nvme_cb, ctx);

	return ret;
}

/*
 * [한국어]
 * nvme_rpc_io_cmd_bdev_nvme - IO qpair로 raw NVMe 명령 발행.
 *
 * @ctx: RPC 컨텍스트.
 * @cmd: SQE 사본.
 * @buf, @nbytes: 데이터 버퍼/길이.
 * @md_buf, @md_len: metadata 버퍼/길이 (DIF/DIX).
 * @timeout_ms: 미사용.
 * @return: 0 성공, 음수 errno.
 *
 * 동작:
 *   1) spdk_get_io_channel로 컨트롤러의 IO 채널 획득 (ref++, qpair 사용권).
 *   2) bdev_nvme_get_io_qpair로 그 채널이 보유한 spdk_nvme_qpair 추출.
 *   3) raw_with_md API로 SQE enqueue + doorbell. DMA는 hugepage 정렬 buf/md를 PRP/SGL로 사용.
 *   4) 발행 실패 시 채널 참조 즉시 해제 (성공 시는 cb에서 해제).
 * 실행 컨텍스트: app 스레드 (cmd 발행), completion은 같은 스레드의 poll group poller.
 */
static int
nvme_rpc_io_cmd_bdev_nvme(struct rpc_bdev_nvme_send_cmd_ctx *ctx, struct spdk_nvme_cmd *cmd,
			  void *buf, uint32_t nbytes, void *md_buf, uint32_t md_len,
			  uint32_t timeout_ms)
{
	struct nvme_ctrlr *_nvme_ctrlr = ctx->nvme_ctrlr;   /* [한국어] 대상 컨트롤러. */
	struct spdk_nvme_qpair *io_qpair;                    /* [한국어] 채널의 IO qpair 핸들. */
	int ret;                                              /* [한국어] 발행 결과. */

	/* [한국어] 채널 획득: 채널은 IO device(=nvme_ctrlr)와 현재 SPDK 스레드 한 쌍에 1개. */
	ctx->ctrlr_io_ch = spdk_get_io_channel(_nvme_ctrlr);
	io_qpair = bdev_nvme_get_io_qpair(ctx->ctrlr_io_ch);   /* [한국어] 채널 컨텍스트에서 qpair 추출. */

	/* [한국어] raw IO 발행 (with metadata variant). PRP/SGL 자동 구성. */
	ret = spdk_nvme_ctrlr_cmd_io_raw_with_md(_nvme_ctrlr->ctrlr, io_qpair,
			cmd, buf, nbytes, md_buf, nvme_rpc_bdev_nvme_cb, ctx);
	if (ret) {
		/* [한국어] 발행 실패 → 채널 즉시 해제 (cb는 호출되지 않음). */
		spdk_put_io_channel(ctx->ctrlr_io_ch);
	}

	return ret;

}

/*
 * [한국어]
 * rpc_bdev_nvme_send_cmd_exec - cmd_type에 따라 admin/io 발행 함수 선택.
 *
 * @ctx: 디코딩 완료된 컨텍스트.
 * @return: 발행 결과 (0 성공, 음수 errno).
 *
 * default 케이스가 없어 잘못된 cmd_type이면 -EINVAL이 그대로 반환된다.
 * (디코더가 admin/io만 허용하므로 실제 도달 불가능에 가깝다.)
 */
static int
rpc_bdev_nvme_send_cmd_exec(struct rpc_bdev_nvme_send_cmd_ctx *ctx)
{
	struct rpc_bdev_nvme_send_cmd_req *req = &ctx->req;   /* [한국어] 디코딩된 요청. */
	int ret = -EINVAL;                                     /* [한국어] 기본값: 알 수 없는 type이면 invalid. */

	switch (req->cmd_type) {
	case NVME_ADMIN_CMD:
		/* [한국어] Admin queue 발행 (qpair는 컨트롤러의 ASQ). */
		ret = nvme_rpc_admin_cmd_bdev_nvme(ctx, req->cmdbuf, req->data,
						   req->data_len, req->timeout_ms);
		break;
	case NVME_IO_CMD:
		/* [한국어] IO qpair 발행 (현재 스레드의 채널 사용). */
		ret = nvme_rpc_io_cmd_bdev_nvme(ctx, req->cmdbuf, req->data,
						req->data_len, req->md, req->md_len, req->timeout_ms);
		break;
	}

	return ret;
}

/*
 * [한국어]
 * rpc_decode_cmd_type - "admin"/"io" 문자열을 enum spdk_nvme_rpc_type으로 변환.
 *
 * @val: JSON 값 (string 기대).
 * @out: int * (NVME_ADMIN_CMD 또는 NVME_IO_CMD).
 * @return: 0 성공, -EINVAL 잘못된 문자열.
 *
 * spdk_json_strequal은 JSON 문자열 토큰과 직접 비교 (NUL 종결 비의존).
 */
static int
rpc_decode_cmd_type(const struct spdk_json_val *val, void *out)
{
	int *cmd_type = out;

	if (spdk_json_strequal(val, "admin") == true) {
		*cmd_type = NVME_ADMIN_CMD;
	} else if (spdk_json_strequal(val, "io") == true) {
		*cmd_type = NVME_IO_CMD;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: cmd_type\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * rpc_decode_data_direction - "h2c"/"c2h" 문자열을 SPDK_NVME_DATA_* enum으로 변환.
 *
 * h2c: Host→Controller (Write 등). c2h: Controller→Host (Read 등).
 * NVMe spec FUSE/NDM과 무관한 사용자 향 표기법.
 */
static int
rpc_decode_data_direction(const struct spdk_json_val *val, void *out)
{
	int *data_direction = out;

	if (spdk_json_strequal(val, "h2c") == true) {
		*data_direction = SPDK_NVME_DATA_HOST_TO_CONTROLLER;
	} else if (spdk_json_strequal(val, "c2h") == true) {
		*data_direction = SPDK_NVME_DATA_CONTROLLER_TO_HOST;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: data_direction\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * [한국어]
 * rpc_decode_cmdbuf - base64 인코딩된 NVMe SQE를 디코딩해서 64바이트 cmdbuf로 복원.
 *
 * @val: JSON 문자열 (base64 url-safe).
 * @out: struct spdk_nvme_cmd ** (할당된 cmdbuf 포인터를 채워줌).
 * @return: 0 성공, -ENOMEM/-EINVAL.
 *
 * 디코딩 결과 길이가 sizeof(spdk_nvme_cmd)(=64)와 일치해야 함. cmdbuf는 호출자(=ctx)
 * 가 free 책임 (free_rpc_bdev_nvme_send_cmd_ctx에서 free).
 */
static int
rpc_decode_cmdbuf(const struct spdk_json_val *val, void *out)
{
	char *text = NULL;                                       /* [한국어] base64 텍스트 (strdup 결과). */
	size_t text_strlen, raw_len;                              /* [한국어] 인코딩/디코딩 길이. */
	struct spdk_nvme_cmd *cmdbuf, **_cmdbuf = out;           /* [한국어] 출력 슬롯. */
	int rc;

	/* [한국어] 1단계: JSON 문자열 → C 문자열 strdup. */
	rc = spdk_json_decode_string(val, &text);
	if (rc) {
		/* [한국어] string이 아닌 타입이면 -EINVAL, string인데 ENOMEM이면 그대로. */
		return val->type == SPDK_JSON_VAL_STRING ? -ENOMEM : -EINVAL;
	}

	text_strlen = strlen(text);
	raw_len = spdk_base64_get_decoded_len(text_strlen);   /* [한국어] base64 디코딩 후 예상 길이. */
	cmdbuf = malloc(raw_len);                              /* [한국어] cmdbuf 할당 (대략 64바이트). */
	if (!cmdbuf) {
		rc = -ENOMEM;
		goto out;
	}

	/* [한국어] 2단계: base64 → 바이너리 디코딩. raw_len은 실제 디코딩 길이로 갱신됨. */
	rc = spdk_base64_urlsafe_decode(cmdbuf, &raw_len, text);
	if (rc) {
		free(cmdbuf);
		goto out;
	}
	/* [한국어] NVMe SQE는 정확히 64바이트여야 함. 다르면 잘못된 입력. */
	if (raw_len != sizeof(*cmdbuf)) {
		rc = -EINVAL;
		free(cmdbuf);
		goto out;
	}

	*_cmdbuf = cmdbuf;   /* [한국어] 호출자 ctx에 cmdbuf 포인터 저장. */

out:
	free(text);   /* [한국어] strdup된 base64 텍스트 해제. */
	return rc;
}

/*
 * [한국어]
 * rpc_decode_data - "data" 필드 디코더 (base64 → DMA 가능 hugepage 버퍼).
 *
 * @val: JSON base64 문자열.
 * @out: struct rpc_bdev_nvme_send_cmd_req * (구조체 자체 - 다른 디코더가 채운 data_len 참조 필요).
 * @return: 0 성공, 음수 errno.
 *
 * 디코더 호출 순서가 보장되지 않으므로 두 가지 시나리오 처리:
 *   - data_len이 먼저 디코딩 → req->data가 이미 할당됨, 길이 검증만 하고 디코딩.
 *   - data가 먼저 디코딩 → 텍스트 길이로 data_len 계산 후 hugepage 할당.
 * 버퍼 크기는 최소 4KB(0x1000)로 보장 (NVMe PRP 정렬 페이지 크기).
 */
static int
rpc_decode_data(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_nvme_send_cmd_req *req = (struct rpc_bdev_nvme_send_cmd_req *)out;
	char *text = NULL;       /* [한국어] base64 텍스트. */
	size_t text_strlen;       /* [한국어] base64 길이. */
	int rc;

	/* [한국어] base64 텍스트 받아오기. */
	rc = spdk_json_decode_string(val, &text);
	if (rc) {
		return val->type == SPDK_JSON_VAL_STRING ? -ENOMEM : -EINVAL;
	}
	text_strlen = strlen(text);

	if (req->data_len) {
		/* data_len is decoded by param "data_len" */
		/* [한국어] data_len이 먼저 들어왔으면 길이 일관성 검증.
		 * base64 디코딩 길이와 명시적 data_len이 다르면 입력 오류. */
		if (req->data_len != spdk_base64_get_decoded_len(text_strlen)) {
			rc = -EINVAL;
			goto out;
		}
	} else {
		/* [한국어] data가 먼저 들어왔다 → 텍스트 길이로 data_len 결정 + hugepage 할당.
		 * spdk_malloc(SPDK_MALLOC_DMA) → DPDK hugepage에서 4KB 정렬, NVMe DMA 가능.
		 * 최소 0x1000(4KB) 보장: NVMe Read/Write 등은 페이지 단위 PRP를 사용. */
		req->data_len = spdk_base64_get_decoded_len(text_strlen);
		req->data = spdk_malloc(req->data_len > 0x1000 ? req->data_len : 0x1000, 0x1000,
					NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!req->data) {
			rc = -ENOMEM;
			goto out;
		}
	}

	/* [한국어] base64 → 바이너리 디코딩 (req->data 버퍼에 직접). */
	rc = spdk_base64_urlsafe_decode(req->data, (size_t *)&req->data_len, text);

out:
	free(text);   /* [한국어] strdup된 base64 텍스트 해제. */
	return rc;
}

/*
 * [한국어]
 * rpc_decode_data_len - "data_len" 필드 디코더 (uint32 → 버퍼 할당 트리거).
 *
 * data 디코더와 짝을 이뤄 두 경로 모두에서 동작. 길이 검증 또는 선할당.
 */
static int
rpc_decode_data_len(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_nvme_send_cmd_req *req = (struct rpc_bdev_nvme_send_cmd_req *)out;
	uint32_t data_len;
	int rc;

	rc = spdk_json_decode_uint32(val, &data_len);
	if (rc) {
		return rc;
	}

	if (req->data_len) {
		/* data_len is decoded by param "data" */
		/* [한국어] data가 먼저 디코딩되었다면 일관성 체크. */
		if (req->data_len != data_len) {
			rc = -EINVAL;
		}
	} else {
		/* [한국어] data가 아직이면 미리 hugepage 버퍼 할당. */
		req->data_len = data_len;
		req->data = spdk_malloc(req->data_len > 0x1000 ? req->data_len : 0x1000, 0x1000,
					NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!req->data) {
			rc = -ENOMEM;
		}
	}

	return rc;
}

/*
 * [한국어]
 * rpc_decode_metadata - "metadata" 필드 디코더. data 버전과 거의 동일.
 *
 * 차이: 최소 크기 보장 없음 (metadata는 PI 8바이트 등 작은 크기일 수 있음). 4KB 정렬은 동일.
 */
static int
rpc_decode_metadata(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_nvme_send_cmd_req *req = (struct rpc_bdev_nvme_send_cmd_req *)out;
	char *text = NULL;
	size_t text_strlen;
	int rc;

	rc = spdk_json_decode_string(val, &text);
	if (rc) {
		return val->type == SPDK_JSON_VAL_STRING ? -ENOMEM : -EINVAL;
	}
	text_strlen = strlen(text);

	if (req->md_len) {
		/* md_len is decoded by param "metadata_len" */
		/* [한국어] md_len이 먼저 → 일관성 검증. */
		if (req->md_len != spdk_base64_get_decoded_len(text_strlen)) {
			rc = -EINVAL;
			goto out;
		}
	} else {
		/* [한국어] metadata가 먼저 → 길이 결정 + DMA 버퍼 할당 (4KB 정렬). */
		req->md_len = spdk_base64_get_decoded_len(text_strlen);
		req->md = spdk_malloc(req->md_len, 0x1000, NULL,
				      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!req->md) {
			rc = -ENOMEM;
			goto out;
		}
	}

	rc = spdk_base64_urlsafe_decode(req->md, (size_t *)&req->md_len, text);

out:
	free(text);
	return rc;
}

/*
 * [한국어]
 * rpc_decode_metadata_len - "metadata_len" 필드 디코더. data_len 버전과 동일 패턴.
 */
static int
rpc_decode_metadata_len(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_nvme_send_cmd_req *req = (struct rpc_bdev_nvme_send_cmd_req *)out;
	uint32_t md_len;
	int rc;

	rc = spdk_json_decode_uint32(val, &md_len);
	if (rc) {
		return rc;
	}

	if (req->md_len) {
		/* md_len is decoded by param "metadata" */
		/* [한국어] metadata가 먼저 → 일관성 검증. */
		if (req->md_len != md_len) {
			rc = -EINVAL;
		}
	} else {
		/* [한국어] metadata가 아직 → 선할당. */
		req->md_len = md_len;
		req->md = spdk_malloc(req->md_len, 0x1000, NULL,
				      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!req->md) {
			rc = -ENOMEM;
		}
	}

	return rc;
}

/*
 * [한국어] bdev_nvme_send_cmd RPC의 입력 디코더 테이블.
 *
 * 일부 필드(timeout_ms, data_len, metadata_len, data, metadata)는 optional=true.
 * 마지막 4개는 offsetof 0인데, 디코더가 req 구조체 자체를 out으로 받아 내부에서 멤버에 직접 접근하기 때문.
 */
static const struct spdk_json_object_decoder rpc_bdev_nvme_send_cmd_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_send_cmd_req, name), spdk_json_decode_string},
	{"cmd_type", offsetof(struct rpc_bdev_nvme_send_cmd_req, cmd_type), rpc_decode_cmd_type},
	{"data_direction", offsetof(struct rpc_bdev_nvme_send_cmd_req, data_direction), rpc_decode_data_direction},
	{"cmdbuf", offsetof(struct rpc_bdev_nvme_send_cmd_req, cmdbuf), rpc_decode_cmdbuf},
	{"timeout_ms", offsetof(struct rpc_bdev_nvme_send_cmd_req, timeout_ms), spdk_json_decode_uint32, true},
	{"data_len", 0, rpc_decode_data_len, true},
	{"metadata_len", 0, rpc_decode_metadata_len, true},
	{"data", 0, rpc_decode_data, true},
	{"metadata", 0, rpc_decode_metadata, true},
};

/*
 * [한국어]
 * rpc_bdev_nvme_send_cmd - "bdev_nvme_send_cmd" RPC 진입점.
 *
 * @request, @params: SPDK JSON-RPC 표준 인자.
 * @return: 없음. 응답은 비동기 cb에서 또는 즉시 invalid 분기에서 전송.
 *
 * 동작:
 *   1) ctx 힙 할당 (completion까지 살아있어야 함).
 *   2) JSON 디코드 (req에 cmdbuf/data/md 채움).
 *   3) nvme_ctrlr lookup.
 *   4) exec()로 admin/io 분기 발행.
 *   5) 발행 성공이면 즉시 return (응답은 cb에서); 실패면 invalid: ctx 해제 + 에러 응답.
 * 실행 컨텍스트: SPDK app 스레드 (RPC 콜백).
 *
 * 호출 체인:
 *   JSON-RPC server → [본 함수] → exec → admin/io raw → 디바이스 → cb → complete → 응답.
 */
static void
rpc_bdev_nvme_send_cmd(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_send_cmd_ctx *ctx;   /* [한국어] 비동기 컨텍스트 (힙). */
	int ret, error_code;                       /* [한국어] errno 및 RPC 에러 코드. */

	/* [한국어] 1단계: ctx 0-init 할당. NULL이면 OOM 응답. */
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed at Malloc ctx\n");
		error_code = SPDK_JSONRPC_ERROR_INTERNAL_ERROR;
		ret = -ENOMEM;
		goto invalid;
	}

	/* [한국어] 2단계: 입력 디코딩. cmdbuf/data/md 등 자동으로 할당됨. */
	if (spdk_json_decode_object(params, rpc_bdev_nvme_send_cmd_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_send_cmd_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		error_code = SPDK_JSONRPC_ERROR_INVALID_PARAMS;
		ret = -EINVAL;
		goto invalid;
	}

	/* [한국어] 3단계: 컨트롤러 lookup (이름 매치). */
	ctx->nvme_ctrlr = nvme_ctrlr_get_by_name(ctx->req.name);
	if (ctx->nvme_ctrlr == NULL) {
		SPDK_ERRLOG("Failed at device lookup\n");
		error_code = SPDK_JSONRPC_ERROR_INVALID_PARAMS;
		ret = -EINVAL;
		goto invalid;
	}

	/* [한국어] 4단계: 응답 전송용 request 핸들 보관 (cb에서 사용). */
	ctx->jsonrpc_request = request;

	/* [한국어] 5단계: 실제 NVMe 명령 발행. 성공 시 cb까지 ctx 유지. */
	ret = rpc_bdev_nvme_send_cmd_exec(ctx);
	if (ret < 0) {
		SPDK_NOTICELOG("Failed at rpc_bdev_nvme_send_cmd_exec\n");
		error_code = SPDK_JSONRPC_ERROR_INTERNAL_ERROR;
		goto invalid;
	}

	/* [한국어] 발행 성공 → cb이 응답 처리, 여기서는 그냥 반환. */
	return;

invalid:
	/* [한국어] 어떤 단계에서든 실패 시 도달. ctx가 있으면 해제 후 에러 응답 전송. */
	if (ctx != NULL) {
		free_rpc_bdev_nvme_send_cmd_ctx(ctx);
	}
	spdk_jsonrpc_send_error_response(request, error_code, spdk_strerror(-ret));
}
SPDK_RPC_REGISTER("bdev_nvme_send_cmd", rpc_bdev_nvme_send_cmd, SPDK_RPC_RUNTIME)
