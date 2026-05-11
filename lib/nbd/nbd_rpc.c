/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] NBD(Network Block Device) JSON-RPC 핸들러 (nbd_rpc.c)
 *
 * === 파일의 역할 ===
 * SPDK NBD 기능을 외부에서 제어할 수 있도록 세 개의 RPC 메서드를 제공한다:
 *  - "nbd_start_disk" : 지정된 SPDK bdev를 /dev/nbdN으로 노출시키기 시작.
 *  - "nbd_stop_disk"  : 노출된 /dev/nbdN을 안전하게 disconnect.
 *  - "nbd_get_disks"  : 현재 등록된 nbd 디스크 목록 조회.
 * 부가적으로 nbd_device 인자가 생략된 경우(자동 할당)에는 /dev/nbd0, /dev/nbd1 ... 순으로
 * 사용 가능한 슬롯을 찾아 부여한다 (find_available_nbd_disk + check_available_nbd_disk).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   사용자 rpc.py / 매니저
 *     → JSON-RPC 서버
 *       → 본 파일의 핸들러
 *         → spdk_nbd_start (lib/nbd/nbd.c)
 *           → 커널 nbd 모듈에 NBD_SET_SOCK 등 ioctl
 *             → /dev/nbdN ↔ SPDK 사이 socketpair 연결 → SPDK가 NBD 프로토콜 서비스 시작
 * 핵심 자원: /sys/block/nbdN/pid (커널 nbd가 사용 중일 때만 존재 — 이를 사용 가능 여부 판정에 사용),
 *          그리고 lib/nbd/nbd.c의 spdk_nbd_disk 전역 리스트.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: linux/nbd.h(NBD ioctl 정의), spdk/rpc, spdk/string, lib/nbd/nbd.c (start/stop/find).
 * - 의존받음: 사용자 RPC 클라이언트.
 * - 데이터 흐름: 클라이언트 입력 JSON → 디코드 → 사전 검사(파일/등록/PID) →
 *   spdk_nbd_start (비동기 콜백) → 완료 시 응답 JSON 작성.
 * - 동기화 주의: nbd_disconnect는 커널이 in-flight I/O flush까지 블록하므로 별도 pthread에서 실행.
 *
 * === 주요 함수/구조체 요약 ===
 * - check_available_nbd_disk : 경로 형식 검증 + SPDK 등록 여부 + /sys/block/nbdN/pid 검사로 사용 가능성 판단.
 * - find_available_nbd_disk  : 인덱스부터 시작해 사용 가능한 첫 /dev/nbdN을 찾음.
 * - rpc_start_nbd_done       : spdk_nbd_start 비동기 완료 콜백 — 자동 할당 모드면 EBUSY 시 다음 슬롯 재시도.
 * - rpc_nbd_start_disk       : "nbd_start_disk" 핸들러.
 * - nbd_disconnect_thread    : NBD_DISCONNECT ioctl이 블록할 수 있어 분리된 pthread로 실행.
 * - rpc_nbd_stop_disk        : "nbd_stop_disk" 핸들러.
 * - rpc_dump_nbd_info        : 디스크 한 개를 JSON 객체로 직렬화.
 * - rpc_nbd_get_disks        : "nbd_get_disks" 핸들러.
 */

#include "spdk/string.h" /* [한국어] spdk_strerror 등 문자열 유틸. */
#include "spdk/env.h"    /* [한국어] DPDK env wrapper (현재 파일에선 직접 호출 없으나 다른 헤더 의존). */
#include "spdk/rpc.h"    /* [한국어] SPDK_RPC_REGISTER 매크로. */
#include "spdk/util.h"   /* [한국어] SPDK_COUNTOF, offsetof. */

#include <linux/nbd.h>   /* [한국어] 커널 NBD ioctl/매크로 정의 (NBD_DISCONNECT 등). */

#include "nbd_internal.h" /* [한국어] 같은 모듈의 내부 API (nbd_disk_find_by_nbd_path 등). */
#include "spdk/log.h"     /* [한국어] SPDK_ERRLOG/INFOLOG/DEBUGLOG. */

struct rpc_nbd_start_disk {
	/* [한국어] "nbd_start_disk" RPC의 비동기 ctx — 시작 완료 콜백까지 살아있어야 한다.
	 * 자동 할당 모드에서 EBUSY 시 다음 인덱스로 재시도하기 위해 nbd_idx까지 보관. */

	char *bdev_name;
	/* [한국어] 사용자가 노출하길 원하는 SPDK bdev의 이름 (예: "Nvme0n1").
	 * 설정자: JSON 디코더(strdup). 해제: free_rpc_nbd_start_disk. */

	char *nbd_device;
	/* [한국어] 사용자가 지정했거나 자동 할당된 /dev/nbdN 경로.
	 * 자동 할당 시 EBUSY 충돌 시점에 free 후 다음 슬롯 문자열로 교체될 수 있음. */

	/* Used to search one available nbd device */
	int nbd_idx;
	/* [한국어] 다음에 시도할 /dev/nbdX의 X 값.
	 * 자동 할당 시 0부터 증가, 사용자 지정 시는 사용되지 않음. */

	bool nbd_idx_specified;
	/* [한국어] 사용자가 nbd_device를 명시했는지 여부.
	 * true: 실패 시 그대로 에러 응답. false: EBUSY면 다음 슬롯으로 재시도. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 비동기 콜백에서 응답을 보낼 RPC 요청 객체.
	 * 라이프타임: SPDK JSON-RPC 서버가 응답을 보낼 때까지 보유. */
};

/*
 * [한국어]
 * free_rpc_nbd_start_disk - rpc_nbd_start_disk ctx의 모든 동적 자원과 자신을 해제.
 *
 * @req: 해제 대상 ctx (NULL 금지).
 *
 * spdk_nbd_start 콜백 또는 에러 경로에서 호출.
 */
static void
free_rpc_nbd_start_disk(struct rpc_nbd_start_disk *req)
{
	free(req->bdev_name); /* [한국어] strdup된 bdev 이름 해제. NULL이면 free는 no-op. */
	free(req->nbd_device); /* [한국어] strdup/find_available_nbd_disk가 만든 nbd 경로 해제. */
	free(req); /* [한국어] ctx 본체 해제. 호출 후 req는 dangling. */
}

static const struct spdk_json_object_decoder rpc_nbd_start_disk_decoders[] = {
	/* [한국어] start_disk RPC 입력 JSON 필드 → ctx 멤버 매핑.
	 * 두 번째 인자 "nbd_device"는 마지막 true로 optional 표시 — 자동 할당 가능. */
	{"bdev_name", offsetof(struct rpc_nbd_start_disk, bdev_name), spdk_json_decode_string},
	/* [한국어] "bdev_name"(필수) → strdup해 ctx에 저장. */
	{"nbd_device", offsetof(struct rpc_nbd_start_disk, nbd_device), spdk_json_decode_string, true},
	/* [한국어] "nbd_device"(선택) → strdup. 미지정 시 NULL 유지. */
};

/* Return 0 to indicate the nbd_device might be available,
 * or non-zero to indicate the nbd_device is invalid or in use.
 */
/*
 * [한국어]
 * check_available_nbd_disk - "/dev/nbdN" 경로가 형식적으로 유효하고 현재 사용 가능한지 확인.
 *
 * @nbd_device: "/dev/nbdN" 형태 경로.
 * @return    : 0 = 사용 가능 가능성 있음(고정 보장 아님), -EBUSY = 사용 중,
 *              그 외 -errno = sscanf/open 실패.
 *
 * 검사 단계:
 *  1) sscanf로 "/dev/nbd<u32>" 형식 + tail이 없는지 확인.
 *  2) SPDK 자체에 이미 등록된 nbd_device인지(nbd_disk_find_by_nbd_path)로 확인.
 *  3) /sys/block/nbdN/pid가 존재하면 커널 nbd가 사용 중 → EBUSY.
 *     ENOENT면 가용. 그 외 errno면 정책상 실패.
 *
 * 실행 컨텍스트: RPC 서버 thread.
 */
static int
check_available_nbd_disk(char *nbd_device)
{
	char nbd_block_path[256]; /* [한국어] /sys/block/nbdN/pid 경로 빌드용 임시 버퍼. */
	char tail[2]; /* [한국어] sscanf의 tail 캡처 — 경로에 추가 문자가 붙으면 매치되어 거부. */
	int rc; /* [한국어] sscanf/open 결과 임시 변수. */
	unsigned int nbd_idx; /* [한국어] 파싱된 N(인덱스) — /sys 경로 빌드에 사용. */
	struct spdk_nbd_disk *nbd; /* [한국어] SPDK 등록 검색 결과. */

	/* nbd device path must be in format of /dev/nbd<num>, with no tail. */
	rc = sscanf(nbd_device, "/dev/nbd%u%1s", &nbd_idx, tail);
	/* [한국어] %u 매칭(1) + 그 뒤 %1s 매칭(2) — 정확히 1개만 매치되면 tail 없는 정상 형식.
	 * 2개 매치되면 "/dev/nbd0X" 같은 잘못된 입력이므로 거부. */
	if (rc != 1) {
		/* [한국어] 형식 오류 — sscanf 자체 실패(0/EOF) 또는 tail 매치(2)인 경우. */
		return -errno;
	}

	/* make sure nbd_device is not registered inside SPDK */
	nbd = nbd_disk_find_by_nbd_path(nbd_device);
	/* [한국어] SPDK 안에서 이미 같은 경로로 nbd 디스크가 등록되어 있으면 EBUSY 처리. */
	if (nbd) {
		/* nbd_device is in use */
		return -EBUSY;
	}

	/* A valid pid file in /sys/block indicates the device is in use */
	snprintf(nbd_block_path, 256, "/sys/block/nbd%u/pid", nbd_idx);
	/* [한국어] 커널 nbd는 클라이언트가 연결되면 /sys/block/nbdN/pid를 만들고 닫히면 제거.
	 * 그 존재 여부로 다른 프로세스의 사용을 판정한다. */

	rc = open(nbd_block_path, O_RDONLY);
	/* [한국어] 파일을 읽기 전용으로 열어본다 — 존재하면 사용 중. */
	if (rc < 0) {
		if (errno == ENOENT) {
			/* nbd_device might be available */
			/* [한국어] pid 파일이 없으면 커널 nbd가 idle 상태 — 가용으로 간주.
			 * "might"라고 표현한 이유: 다른 RPC가 동시에 같은 슬롯을 점유할 수 있는 race가 미세하게 존재. */
			return 0;
		} else {
			/* [한국어] 권한 부족(EACCES) 등 다른 에러 — 보수적으로 실패 처리. */
			SPDK_ERRLOG("Failed to check PID file %s: %s\n", nbd_block_path, spdk_strerror(errno));
			return -errno;
		}
	}

	close(rc); /* [한국어] open 성공했으면 fd close — 우리가 실제로 잡고 있을 필요 없음. */

	/* nbd_device is in use */
	return -EBUSY; /* [한국어] pid 파일 존재 = 다른 호스트가 사용 중. */
}

/*
 * [한국어]
 * find_available_nbd_disk - 인덱스 nbd_idx부터 시작해 가장 빠른 가용 /dev/nbdN을 검색.
 *
 * @nbd_idx     : 검색을 시작할 인덱스 (자동 할당 시 0, 재시도 시 직전 인덱스+1).
 * @next_nbd_idx: 다음 재시도용 인덱스 출력 (NULL 허용).
 * @return      : 가용 슬롯이 있으면 strdup된 "/dev/nbdN" 문자열, 없으면 NULL.
 *
 * /dev/nbdN이 access()로 존재하는 동안만 시도하고, 존재하지 않는 인덱스에 도달하면
 * 커널이 더 이상 nbd 슬롯을 만들지 않은 상태로 보고 검색을 종료. 즉, 사용 가능한 끝까지 탐색.
 *
 * 호출 체인:
 *   rpc_nbd_start_disk → [find_available_nbd_disk] → check_available_nbd_disk
 */
static char *
find_available_nbd_disk(int nbd_idx, int *next_nbd_idx)
{
	int i, rc; /* [한국어] 루프 인덱스/반환값 임시. */
	char nbd_device[20]; /* [한국어] "/dev/nbdNNN" 까지 안전한 작은 버퍼. */

	for (i = nbd_idx; ; i++) {
		/* [한국어] nbd_idx부터 시작해 무한 증가 — 종료는 access() 실패로 break. */
		snprintf(nbd_device, 20, "/dev/nbd%d", i); /* [한국어] /dev/nbdN 경로 빌드. */
		/* Check whether an nbd device exists in order to reach the last one nbd device */
		rc = access(nbd_device, F_OK);
		/* [한국어] 파일 존재 여부 확인 — 존재하지 않으면 커널이 더 이상 nbd 인스턴스를
		 * 만들어두지 않은 상태로 보고 루프 종료. nbd 모듈은 nbds_max 만큼 미리 만들어둔다. */
		if (rc != 0) {
			break;
		}

		rc = check_available_nbd_disk(nbd_device);
		/* [한국어] 형식 + SPDK/커널 점유 검사를 통과하면 사용 가능. */
		if (rc == 0) {
			if (next_nbd_idx != NULL) {
				*next_nbd_idx = i + 1; /* [한국어] 다음 재시도 시 사용할 인덱스 힌트 저장. */
			}

			return strdup(nbd_device); /* [한국어] 호출자가 free 책임. */
		}
		/* [한국어] EBUSY 등이면 다음 인덱스로 계속 진행. */
	}

	return NULL; /* [한국어] 루프 끝까지 가용 슬롯 못 찾음. */
}

/*
 * [한국어]
 * rpc_start_nbd_done - spdk_nbd_start 비동기 완료 콜백.
 *
 * @cb_arg: 등록 시 전달한 rpc_nbd_start_disk ctx.
 * @nbd   : 성공 시 생성된 spdk_nbd_disk 핸들 (실패 시 의미 없음).
 * @rc    : 0 성공, 음수 errno 실패.
 *
 * 자동 할당 모드에서 -EBUSY가 나오면 race 등으로 슬롯이 사라진 상황 — 다음 가용 슬롯을
 * 찾아 spdk_nbd_start를 다시 호출하고 콜백을 자기 자신으로 등록(꼬리재호출 패턴).
 * 결국 성공/모든 슬롯 실패 중 하나로 수렴하면 응답을 보내고 ctx를 해제한다.
 *
 * 실행 컨텍스트: nbd 모듈 내부 polling thread(보통 마스터 코어).
 */
static void
rpc_start_nbd_done(void *cb_arg, struct spdk_nbd_disk *nbd, int rc)
{
	struct rpc_nbd_start_disk *req = cb_arg; /* [한국어] 비동기 ctx 복원. */
	struct spdk_jsonrpc_request *request = req->request; /* [한국어] 응답을 보낼 RPC request. */
	struct spdk_json_write_ctx *w; /* [한국어] 성공 응답 JSON 빌더. */

	/* Check whether it's automatic nbd-device assignment */
	if (rc == -EBUSY && req->nbd_idx_specified == false) {
		/* [한국어] 자동 할당 모드 + 같은 슬롯 동시 점유 race → 다음 슬롯 시도. */
		free(req->nbd_device); /* [한국어] 이전에 시도한 경로 메모리 해제. */

		req->nbd_device = find_available_nbd_disk(req->nbd_idx, &req->nbd_idx);
		/* [한국어] 다음 인덱스 검색 — 동시에 nbd_idx를 다음 시도용으로 갱신. */
		if (req->nbd_device != NULL) {
			spdk_nbd_start(req->bdev_name, req->nbd_device,
				       rpc_start_nbd_done, req);
			/* [한국어] 새 슬롯으로 비동기 start 재요청 — 본 콜백이 다시 호출됨. */
			return;
		}

		SPDK_INFOLOG(nbd, "There is no available nbd device.\n");
		/* [한국어] 더 이상 시도할 슬롯이 없으면 ENODEV로 떨어진다 (아래 if (rc) 분기). */
	}

	if (rc) {
		/* [한국어] 실패 응답 — rc는 음수, JSON-RPC 에러 코드에 그대로 사용. */
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free_rpc_nbd_start_disk(req); /* [한국어] ctx 정리. */
		return;
	}

	w = spdk_jsonrpc_begin_result(request); /* [한국어] 성공 응답 시작. */
	spdk_json_write_string(w, spdk_nbd_get_path(nbd));
	/* [한국어] 응답으로 실제 부여된 "/dev/nbdN" 문자열을 반환 — 자동 할당 시 사용자가 사용할 경로. */
	spdk_jsonrpc_end_result(request, w);

	free_rpc_nbd_start_disk(req); /* [한국어] 성공 후 ctx 해제. */
}

/*
 * [한국어]
 * rpc_nbd_start_disk - "nbd_start_disk" RPC 핸들러.
 *
 * @request: JSON-RPC 요청.
 * @params : { "bdev_name": "<name>", "nbd_device": "/dev/nbdN" (optional) } JSON.
 *
 * 단계:
 *  1) ctx 동적 할당.
 *  2) JSON 디코드 + bdev_name 필수 체크.
 *  3) nbd_device가 명시됐으면 사용 가능성 검사 후 그대로 사용. 미명시면 자동 할당 시도.
 *  4) spdk_nbd_start(bdev, nbd, cb=rpc_start_nbd_done, ctx)로 비동기 시작 (이후 진행은 콜백).
 */
static void
rpc_nbd_start_disk(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_nbd_start_disk *req; /* [한국어] 비동기 콜백까지 살아 있을 ctx. */
	int rc; /* [한국어] check 결과 임시. */

	req = calloc(1, sizeof(*req)); /* [한국어] zero-init된 ctx 동적 할당. */
	if (req == NULL) {
		/* [한국어] OOM 즉시 INTERNAL_ERROR 응답. */
		SPDK_ERRLOG("could not allocate nbd_start_disk request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_nbd_start_disk_decoders,
				    SPDK_COUNTOF(rpc_nbd_start_disk_decoders),
				    req)) {
		/* [한국어] JSON 형식/필드 오류 — 로그 + INTERNAL_ERROR 응답 후 정리 분기로. */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto invalid;
	}

	if (req->bdev_name == NULL) {
		/* [한국어] required 필드인데 NULL — 디코더는 통과했지만 빈 값일 가능성에 대한 방어. */
		goto invalid;
	}

	if (req->nbd_device != NULL) {
		/* [한국어] 사용자가 nbd_device를 명시적으로 지정한 경로. */
		req->nbd_idx_specified = true; /* [한국어] EBUSY 시 자동 재시도 금지 표시. */
		rc = check_available_nbd_disk(req->nbd_device); /* [한국어] 형식+가용성 사전 점검. */
		if (rc == -EBUSY) {
			/* [한국어] 이미 사용 중 — 사용자 잘못된 입력으로 보고 즉시 거부. */
			SPDK_DEBUGLOG(nbd, "NBD device %s is in use.\n", req->nbd_device);
			spdk_jsonrpc_send_error_response(request, -EBUSY, spdk_strerror(-rc));
			goto invalid;
		}

		if (rc != 0) {
			/* [한국어] 형식 오류/권한 오류 등 — ENODEV 류로 응답. */
			SPDK_DEBUGLOG(nbd, "Illegal nbd_device %s.\n", req->nbd_device);
			spdk_jsonrpc_send_error_response_fmt(request, -ENODEV,
							     "illegal nbd device %s", req->nbd_device);
			goto invalid;
		}
	} else {
		/* [한국어] 자동 할당 모드 — 0번부터 가용 슬롯 검색. */
		req->nbd_idx = 0;
		req->nbd_device = find_available_nbd_disk(req->nbd_idx, &req->nbd_idx);
		if (req->nbd_device == NULL) {
			/* [한국어] 시스템에 가용 nbd 슬롯이 없음 — 사용자에게 ENODEV로 안내. */
			SPDK_INFOLOG(nbd, "There is no available nbd device.\n");
			spdk_jsonrpc_send_error_response(request, -ENODEV,
							 "nbd device not found");
			goto invalid;
		}
	}

	req->request = request; /* [한국어] 비동기 콜백에서 응답 보내기 위해 request 저장. */
	spdk_nbd_start(req->bdev_name, req->nbd_device,
		       rpc_start_nbd_done, req);
	/* [한국어] 비동기 NBD 디스크 생성 시작. 완료 시 rpc_start_nbd_done 콜백이 ctx와 결과를 받는다. */

	return;

invalid:
	/* [한국어] 에러 분기 — 응답은 위에서 이미 보냈고 ctx만 해제. */
	free_rpc_nbd_start_disk(req);
}

SPDK_RPC_REGISTER("nbd_start_disk", rpc_nbd_start_disk, SPDK_RPC_RUNTIME)
/* [한국어] "nbd_start_disk" RPC를 RUNTIME 단계에 등록. */

struct rpc_nbd_stop_disk {
	/* [한국어] "nbd_stop_disk" RPC 입력 디코드 컨테이너. 단일 필수 필드. */

	char *nbd_device;
	/* [한국어] disconnect할 "/dev/nbdN" 경로. NULL이면 ENODEV 응답. */
};

/*
 * [한국어]
 * free_rpc_nbd_stop_disk - stop_disk 입력 ctx의 동적 자원 해제 (ctx 본체는 스택).
 */
static void
free_rpc_nbd_stop_disk(struct rpc_nbd_stop_disk *req)
{
	free(req->nbd_device); /* [한국어] strdup된 경로 해제. */
}

static const struct spdk_json_object_decoder rpc_nbd_stop_disk_decoders[] = {
	/* [한국어] "nbd_device" → ctx.nbd_device 매핑. 필수(optional 표시 없음). */
	{"nbd_device", offsetof(struct rpc_nbd_stop_disk, nbd_device), spdk_json_decode_string},
};

struct nbd_disconnect_arg {
	/* [한국어] 분리된 pthread에서 실제 disconnect를 실행하기 위한 인자.
	 * pthread는 detached로 만들어지므로 thread 자체가 자기 자원 해제 책임을 진다. */

	struct spdk_jsonrpc_request *request;
	/* [한국어] 응답할 RPC 요청. 응답을 보내기 전까지 유효. */

	struct spdk_nbd_disk *nbd;
	/* [한국어] disconnect 대상 디스크. RPC 핸들러에서 lookup해 보관. */
};

/*
 * [한국어]
 * nbd_disconnect_thread - 분리 thread에서 NBD_DISCONNECT 처리.
 *
 * @arg: nbd_disconnect_arg* — 자기 자신이 해제 책임.
 *
 * 커널 NBD_DISCONNECT ioctl은 in-flight I/O의 flush를 기다리며 블록할 수 있다. SPDK
 * reactor thread에서 이걸 부르면 polling이 멈추므로, 별도 OS pthread를 띄워 그 안에서
 * 처리한다. spdk_unaffinitize_thread()로 SPDK reactor의 affinity 영향을 제거한다.
 *
 * 실행 컨텍스트: 일반 Linux pthread (SPDK reactor 외).
 */
static void *
nbd_disconnect_thread(void *arg)
{
	struct nbd_disconnect_arg *thd_arg = arg; /* [한국어] 인자 복원. */

	spdk_unaffinitize_thread();
	/* [한국어] DPDK가 reactor thread에 박아둔 CPU affinity를 해제 — OS 스케줄러가 임의 코어로
	 * 옮길 수 있게 해 reactor 코어를 점유하지 않도록 한다. */

	nbd_disconnect(thd_arg->nbd);
	/* [한국어] 실제 NBD_DISCONNECT 호출 — 플러시 완료까지 동기 블록 가능. */

	spdk_jsonrpc_send_bool_response(thd_arg->request, true);
	/* [한국어] 성공 응답 전송. 이 호출은 thread-safe (jsonrpc 서버는 응답 큐에 push). */

	free(thd_arg); /* [한국어] 인자 해제 — 더 이상 사용되지 않음. */
	pthread_exit(NULL); /* [한국어] thread 종료. detached이므로 join 불필요. */
}

/*
 * [한국어]
 * rpc_nbd_stop_disk - "nbd_stop_disk" RPC 핸들러.
 *
 * @request: JSON-RPC 요청.
 * @params : { "nbd_device": "/dev/nbdN" } JSON.
 *
 * 단계:
 *  1) JSON 디코드 + 입력 유효성 검사 (NULL이면 ENODEV).
 *  2) 등록된 nbd 디스크 lookup — 없으면 ENODEV.
 *  3) 분리 pthread로 실제 disconnect 실행 (블로킹 가능 작업이므로).
 */
static void
rpc_nbd_stop_disk(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct rpc_nbd_stop_disk req = {}; /* [한국어] 디코드 결과 — 0 초기화. */
	struct spdk_nbd_disk *nbd; /* [한국어] lookup 결과. */
	pthread_t tid; /* [한국어] 생성된 thread id (detach 후 사용 안 함). */
	struct nbd_disconnect_arg *thd_arg = NULL; /* [한국어] thread 인자. NULL이면 free 불필요. */
	int rc; /* [한국어] pthread_* 결과 임시. */

	if (spdk_json_decode_object(params, rpc_nbd_stop_disk_decoders,
				    SPDK_COUNTOF(rpc_nbd_stop_disk_decoders),
				    &req)) {
		/* [한국어] 디코드 실패 — 입력 형식 오류. */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto out;
	}

	if (req.nbd_device == NULL) {
		/* [한국어] 디코더는 통과했어도 NULL이면 의미 없는 요청 — 거부. */
		spdk_jsonrpc_send_error_response(request, -ENODEV, "invalid nbd device");
		goto out;
	}

	/* make sure nbd_device is registered */
	nbd = nbd_disk_find_by_nbd_path(req.nbd_device);
	/* [한국어] SPDK 측 등록 여부 확인 — 우리가 만든 디스크가 아니면 처리하지 않음. */
	if (!nbd) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto out;
	}

	/*
	 * thd_arg should be freed by created thread
	 * if thread is created successfully.
	 */
	thd_arg = malloc(sizeof(*thd_arg)); /* [한국어] thread에 넘길 ctx 동적 할당. */
	if (!thd_arg) {
		/* [한국어] OOM 즉시 INTERNAL_ERROR 응답. */
		SPDK_ERRLOG("could not allocate nbd disconnect thread arg\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		goto out;
	}

	thd_arg->request = request; /* [한국어] thread가 응답할 대상. */
	thd_arg->nbd = nbd; /* [한국어] disconnect 대상. */

	/*
	 * NBD ioctl of disconnect will block until data are flushed.
	 * Create separate thread to execute it.
	 */
	rc = pthread_create(&tid, NULL, nbd_disconnect_thread, (void *)thd_arg);
	/* [한국어] 분리 thread 생성 — 기본 attribute로 충분. */
	if (rc != 0) {
		/* [한국어] thread 생성 실패 — 인자 해제하고 에러 응답. */
		SPDK_ERRLOG("could not create nbd disconnect thread: %s\n", spdk_strerror(rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(rc));
		free(thd_arg);
		goto out;
	}

	rc = pthread_detach(tid);
	/* [한국어] thread를 detached로 만들어 자원 자동 회수 — RPC 응답은 thread가 직접 보낸다. */
	if (rc != 0) {
		SPDK_ERRLOG("could not detach nbd disconnect thread: %s\n", spdk_strerror(rc));
		/* [한국어] detach 실패 시에도 thread는 이미 동작 중 — 자원 leak 가능성이 있으나
		 * disconnect 자체는 진행되며, 응답도 thread가 보낼 것. RPC 자체에는 별도 응답 안 함. */
		goto out;
	}

out:
	free_rpc_nbd_stop_disk(&req); /* [한국어] 디코드된 nbd_device 문자열 해제. */
}

SPDK_RPC_REGISTER("nbd_stop_disk", rpc_nbd_stop_disk, SPDK_RPC_RUNTIME)
/* [한국어] "nbd_stop_disk" RPC 등록 (RUNTIME 단계). */

/*
 * [한국어]
 * rpc_dump_nbd_info - 한 nbd 디스크의 메타정보를 JSON 객체 한 개로 직렬화.
 *
 * @w  : 진행 중인 JSON write context (배열 안에서 호출).
 * @nbd: 직렬화 대상.
 */
static void
rpc_dump_nbd_info(struct spdk_json_write_ctx *w,
		  struct spdk_nbd_disk *nbd)
{
	spdk_json_write_object_begin(w); /* [한국어] '{' 시작. */

	spdk_json_write_named_string(w, "nbd_device", nbd_disk_get_nbd_path(nbd));
	/* [한국어] "nbd_device": "/dev/nbdN" 출력. */

	spdk_json_write_named_string(w, "bdev_name", nbd_disk_get_bdev_name(nbd));
	/* [한국어] "bdev_name": "<백킹 bdev 이름>" 출력. */

	spdk_json_write_object_end(w); /* [한국어] '}' 종결. */
}

struct rpc_nbd_get_disks {
	/* [한국어] "nbd_get_disks" RPC 입력 — nbd_device 필드는 선택. 지정 시 단일 디스크만 응답. */

	char *nbd_device;
	/* [한국어] 단일 조회 대상 경로(선택). 미지정 시 전체 리스트 반환. */
};

/*
 * [한국어]
 * free_rpc_nbd_get_disks - get_disks 입력 ctx의 동적 필드 해제.
 */
static void
free_rpc_nbd_get_disks(struct rpc_nbd_get_disks *r)
{
	free(r->nbd_device); /* [한국어] strdup된 경로 해제 — NULL이면 no-op. */
}

static const struct spdk_json_object_decoder rpc_nbd_get_disks_decoders[] = {
	/* [한국어] "nbd_device"는 optional(true). 미지정 가능. */
	{"nbd_device", offsetof(struct rpc_nbd_get_disks, nbd_device), spdk_json_decode_string, true},
};

/*
 * [한국어]
 * rpc_nbd_get_disks - "nbd_get_disks" RPC 핸들러 — 등록된 nbd 디스크 목록 응답.
 *
 * @request: JSON-RPC 요청.
 * @params : NULL이거나 { "nbd_device": "/dev/nbdN" } 형태.
 *
 * 동작:
 *  - params=NULL  → 전체 디스크 리스트.
 *  - nbd_device 지정 → 해당 디스크 1개만(없으면 ENODEV).
 *
 * 응답: JSON 배열 [ { "nbd_device": ..., "bdev_name": ... }, ... ].
 */
static void
rpc_nbd_get_disks(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct rpc_nbd_get_disks req = {}; /* [한국어] 입력 디코드용 (옵션 필드 NULL 시작). */
	struct spdk_json_write_ctx *w; /* [한국어] 응답 빌더 컨텍스트. */
	struct spdk_nbd_disk *nbd = NULL; /* [한국어] 단일 모드용 lookup 결과. NULL이면 전체 walk. */

	if (params != NULL) {
		/* [한국어] 인자가 있으면 디코딩 시도. */
		if (spdk_json_decode_object(params, rpc_nbd_get_disks_decoders,
					    SPDK_COUNTOF(rpc_nbd_get_disks_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			goto invalid;
		}

		if (req.nbd_device) {
			/* [한국어] nbd_device 지정 모드 — 정확히 그 경로의 디스크 검색. */
			nbd = nbd_disk_find_by_nbd_path(req.nbd_device);
			if (nbd == NULL) {
				SPDK_ERRLOG("nbd device '%s' does not exist\n", req.nbd_device);
				spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
				goto invalid;
			}

			free_rpc_nbd_get_disks(&req);
			/* [한국어] 입력 메모리는 더 이상 필요 없음 — 정상 흐름에서 미리 해제.
			 * 본 함수는 invalid 경로에서도 free_rpc_nbd_get_disks를 호출하므로
			 * 정상 경로에서 한 번 해제 후 req를 더 이상 참조하지 않는다. */
		}
	}

	w = spdk_jsonrpc_begin_result(request); /* [한국어] 응답 시작. */
	spdk_json_write_array_begin(w); /* [한국어] '[' — 결과는 항상 배열. */

	if (nbd != NULL) {
		/* [한국어] 단일 디스크 모드. */
		rpc_dump_nbd_info(w, nbd);
	} else {
		/* [한국어] 전체 walk — 등록된 모든 디스크를 차례로 직렬화. */
		for (nbd = nbd_disk_first(); nbd != NULL; nbd = nbd_disk_next(nbd)) {
			rpc_dump_nbd_info(w, nbd);
		}
	}

	spdk_json_write_array_end(w); /* [한국어] ']'. */

	spdk_jsonrpc_end_result(request, w); /* [한국어] 응답 송신 마무리. */

	return;

invalid:
	/* [한국어] 에러 분기 — 응답 위에서 보냈고 ctx만 정리. */
	free_rpc_nbd_get_disks(&req);
}
SPDK_RPC_REGISTER("nbd_get_disks", rpc_nbd_get_disks, SPDK_RPC_RUNTIME)
/* [한국어] "nbd_get_disks" RPC 등록 (RUNTIME 단계). */
