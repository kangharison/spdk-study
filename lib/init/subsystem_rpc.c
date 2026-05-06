/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] SPDK 서브시스템 정보 조회 RPC 핸들러 (subsystem_rpc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK 프레임워크의 서브시스템 메타데이터를 외부(스크립트/관리자)로 노출하는
 * 세 개의 JSON-RPC 메서드를 구현한다 — `framework_get_subsystems`,
 * `framework_get_config`, `framework_get_pci_devices`. SPDK 데몬은 부팅 시 다수의
 * 서브시스템(bdev, nvmf, nvme, sock, vhost 등)을 가지고 있으며, 외부 도구
 * (예: `scripts/rpc.py`)가 이 메서드들을 호출하여 (1) 어떤 서브시스템이 등록되어 있고
 * 어떤 의존 관계인지, (2) 각 서브시스템이 만드는 현재 설정(JSON)이 무엇인지,
 * (3) 현재 시스템에 보이는 PCI 디바이스 정보가 무엇인지를 질의한다.
 * 핸들러들은 모두 `SPDK_RPC_RUNTIME` phase 에서만 동작한다 — 즉 STARTUP 단계의
 * 서브시스템 초기화가 완료된 뒤에만 호출 가능하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * RPC 호출 경로: 외부 스크립트(rpc.py) → UNIX 도메인 소켓(/var/tmp/spdk.sock)
 *   → spdk_rpc_server_accept (rpc.c 의 g_rpc_poller) → spdk_jsonrpc_server
 *   → 본 파일의 SPDK_RPC_REGISTER 으로 등록된 핸들러. 결과는 `spdk_jsonrpc_request *`
 *   API 를 통해 동일 소켓으로 직렬화되어 회신된다. 이 모든 핸들러는 SPDK app thread
 *   (마스터 reactor 의 thread) 에서 호출되며, 서브시스템 리스트는 같은 thread 에서만
 *   변경되므로 락 없이 안전하게 순회할 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - subsystem.c: g_subsystems / g_subsystems_deps TAILQ 의 enumerate 함수를 통해
 *   메타데이터를 읽는다 (subsystem_get_first/next, subsystem_get_first_depend/next).
 *   각 서브시스템의 write_config_json 콜백을 subsystem_config_json() 으로 위임 호출한다.
 * - lib/jsonrpc: spdk_jsonrpc_request, spdk_json_write_ctx 등 JSON 직렬화 API.
 * - lib/env_dpdk (PCI): spdk_pci_for_each_device, spdk_pci_device_cfg_read 등을 통해
 *   PCIe BDF 와 256B/4096B configuration space 를 덤프한다 — 외부 진단 도구에 매우 유용.
 * - lib/init/rpc.c: 본 파일의 RPC 핸들러들이 등록될 RPC 서버 자체를 관리하는 모듈.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpc_framework_get_subsystems(): 등록된 모든 서브시스템과 그 depends_on 리스트를
 *   JSON 배열로 반환. 인자 없음.
 * - rpc_framework_get_config(): 단일 서브시스템 이름을 받아 그 서브시스템의
 *   write_config_json() 콜백 결과(현재 런타임 설정)를 JSON 으로 반환.
 * - rpc_framework_get_pci_devices(): 시스템에 보이는 PCI 디바이스(BDF + cfg space)
 *   배열을 반환. 외부 도구의 디버깅·인벤토리 용도.
 * - dump_pci_device(): spdk_pci_for_each_device() 의 콜백, 한 디바이스의 BDF/타입/
 *   config space 를 JSON 객체로 직렬화한다.
 * - rpc_framework_get_config_ctx: get_config 의 디코딩용 임시 컨테이너 (단일 필드 name).
 */

#include "spdk/rpc.h"          /* [한국어] SPDK_RPC_REGISTER 매크로, spdk_jsonrpc_* 송수신 API 정의 */
#include "spdk/string.h"       /* [한국어] 문자열 유틸리티 (spdk_strerror 등) — 본 파일에서는 미사용이나 관례적 포함 */
#include "spdk/util.h"         /* [한국어] SPDK_COUNTOF (배열 길이 매크로) — 디코더 배열 길이 계산에 사용 */
#include "spdk/env.h"          /* [한국어] spdk_pci_* (PCI 디바이스 enumerate, config space 읽기), spdk_mem_all_zero */
#include "spdk/log.h"          /* [한국어] SPDK_ERRLOG 등 — 본 파일에서 직접 사용은 적으나 표준 포함 */

#include "spdk_internal/init.h" /* [한국어] init 라이브러리 내부 API — spdk_subsystem 구조체 정의 등 */

#include "subsystem.h"         /* [한국어] 본 init 라이브러리 내부 헤더 — subsystem_get_first/next, subsystem_find 등 선언 */

/*
 * [한국어]
 * rpc_framework_get_subsystems - "framework_get_subsystems" RPC 핸들러
 *
 * @request: 외부 클라이언트의 RPC 요청 컨텍스트. 응답 회신 시 사용된다.
 * @params: JSON-RPC params 필드. 본 메서드는 인자가 없으므로 NULL 이어야 한다.
 *          NULL 이 아니면 INVALID_PARAMS 에러로 응답한다.
 * @return: void. 결과는 spdk_jsonrpc_end_result/send_error_response 로 비동기 회신.
 *
 * 등록된 모든 서브시스템(g_subsystems TAILQ)을 순회하며 각 서브시스템의 이름과
 * 의존 대상(depends_on) 배열을 JSON 으로 직렬화한다. 출력 스키마:
 *   [ {"subsystem":"<name>", "depends_on":["<dep1>", "<dep2>", ...]}, ... ]
 * 외부 도구(rpc.py)는 이 정보를 토대로 init 순서, 누가 누구에 의존하는지를 파악한다.
 *
 * 실행 컨텍스트: SPDK app thread (master reactor) — RPC poller 가 호출.
 * 동시성: 서브시스템 리스트는 같은 thread 에서만 변경되므로 락 불필요.
 *
 * 호출 체인:
 *   spdk_rpc_server_accept (rpc.c poller) → jsonrpc dispatch → [rpc_framework_get_subsystems]
 *     → subsystem_get_first/next, subsystem_get_first_depend/next (subsystem.c)
 *     → spdk_jsonrpc_end_result (회신)
 */
static void
rpc_framework_get_subsystems(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;          /* [한국어] JSON 직렬화 컨텍스트 — begin_result 가 반환 */
	struct spdk_subsystem *subsystem;       /* [한국어] g_subsystems TAILQ 의 현재 순회 노드 */
	struct spdk_subsystem_depend *deps;     /* [한국어] g_subsystems_deps TAILQ 의 현재 순회 노드 (의존 관계) */

	/* [한국어] params 가 NULL 이 아니면 인자를 받지 않는 메서드인데도 인자가 전달된 케이스 */
	if (params) {
		/* [한국어] JSON-RPC 표준 에러 INVALID_PARAMS(-32602) 로 즉시 회신하고 종료 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'framework_get_subsystems' requires no arguments");
		return;
	}

	/* [한국어] 결과 작성 시작 — 내부적으로 JSON 응답 헤더 ("jsonrpc","id","result":...) 를 준비 */
	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] 결과는 서브시스템 객체들의 JSON 배열 — '[' 시작 토큰 작성 */
	spdk_json_write_array_begin(w);
	/* [한국어] g_subsystems TAILQ 의 첫 노드 가져오기 (위상 정렬된 init 순서대로 노출) */
	subsystem = subsystem_get_first();
	while (subsystem != NULL) {                                  /* [한국어] 모든 서브시스템 순회 */
		spdk_json_write_object_begin(w);                     /* [한국어] 한 서브시스템에 대한 JSON 객체 '{' 시작 */

		spdk_json_write_named_string(w, "subsystem", subsystem->name);  /* [한국어] "subsystem":"<이름>" 키-값 작성 */
		spdk_json_write_named_array_begin(w, "depends_on");  /* [한국어] "depends_on": [ ... ] 배열 시작 */
		deps = subsystem_get_first_depend();                 /* [한국어] 전역 의존성 리스트의 첫 항목 — 모든 (name, depends_on) 쌍을 보관 */
		while (deps != NULL) {                               /* [한국어] 모든 (subsystem, depends_on) 쌍 순회 */
			/* [한국어] 현재 출력 중인 서브시스템 이름과 일치하는 의존성만 골라 출력 */
			if (strcmp(subsystem->name, deps->name) == 0) {
				spdk_json_write_string(w, deps->depends_on);  /* [한국어] depends_on 배열에 의존 대상 이름 추가 */
			}
			deps = subsystem_get_next_depend(deps);      /* [한국어] 다음 의존성 항목으로 진행 */
		}
		spdk_json_write_array_end(w);                        /* [한국어] depends_on 배열 ']' 종결 */
		spdk_json_write_object_end(w);                       /* [한국어] 서브시스템 객체 '}' 종결 */
		subsystem = subsystem_get_next(subsystem);           /* [한국어] 다음 서브시스템으로 이동 */
	}
	spdk_json_write_array_end(w);                                /* [한국어] 최외곽 배열 ']' 종결 */
	spdk_jsonrpc_end_result(request, w);                         /* [한국어] 응답 송출 — 클라이언트 소켓으로 직렬화 전송 */
}

/* [한국어] SPDK_RPC_REGISTER: 컴파일 시점에 constructor attribute 로 RPC 메서드를 자동 등록.
 * "framework_get_subsystems" 라는 이름으로 위 핸들러를 SPDK_RPC_RUNTIME phase 에서만 호출 가능하게 등록.
 * STARTUP phase(서브시스템 초기화 진행 중)에는 호출되지 않으며, INVALID_STATE 로 거부된다. */
SPDK_RPC_REGISTER("framework_get_subsystems", rpc_framework_get_subsystems, SPDK_RPC_RUNTIME)

/* [한국어] framework_get_config 메서드의 입력 인자 임시 보관용 구조체.
 * spdk_json_decode_object 가 디코더 배열의 offsetof 정보를 사용해 본 구조체 멤버에 값을 채워넣는다. */
struct rpc_framework_get_config_ctx {
	char *name;
	/* [한국어] 조회할 서브시스템의 이름 (필수 인자).
	 * 설정자: spdk_json_decode_object → spdk_json_decode_string 이 strdup 으로 할당.
	 * 읽는 자: subsystem_find() 에 넘겨 매칭되는 서브시스템 검색.
	 * 값 범위: 클라이언트가 보낸 임의 문자열 — 서브시스템 이름 문자열 (NUL 종결).
	 * 동기화: 핸들러 함수의 스택 컨텍스트(요청 단위) 외부로 새지 않으므로 동기화 불필요. 사용 후 free() 필요. */
};

/* [한국어] JSON 디코더 테이블 — "name" 필드(string)를 rpc_framework_get_config_ctx::name 에 저장.
 * spdk_json_decode_object 가 이 테이블을 보고 입력 JSON 의 각 키를 구조체 오프셋으로 매핑. */
static const struct spdk_json_object_decoder rpc_framework_get_config_decoders[] = {
	{"name", offsetof(struct rpc_framework_get_config_ctx, name), spdk_json_decode_string},
	/* [한국어] 첫 번째 인자: JSON 키 이름,
	 *         두 번째: 출력 구조체 내 오프셋,
	 *         세 번째: 디코더 함수 (string→char* strdup),
	 *         네 번째 옵션 인자(여기선 생략) = optional 여부 (false=필수). */
};

/*
 * [한국어]
 * rpc_framework_get_config - "framework_get_config" RPC 핸들러
 *
 * @request: RPC 요청 컨텍스트 — 응답 회신용.
 * @params: {"name": "<subsystem_name>"} 형태의 JSON 객체.
 * @return: void. 비동기 응답으로 해당 서브시스템의 write_config_json() 결과를 회신.
 *
 * 외부 도구가 단일 서브시스템의 현재 설정(런타임 시점의 활성 구성)을 JSON 으로 받기
 * 위한 메서드. write_config_json 콜백이 없는 서브시스템은 null 을 반환한다.
 * 일반적인 사용처: 운영 중인 SPDK 프로세스의 설정을 dump 하여 다른 인스턴스에서
 * 재사용하기 위한 구성 파일 생성.
 *
 * 호출 체인:
 *   jsonrpc dispatch → [rpc_framework_get_config]
 *     → spdk_json_decode_object (인자 검증)
 *     → subsystem_find (subsystem.c)
 *     → subsystem_config_json (subsystem.c) → subsystem->write_config_json(w)
 *     → spdk_jsonrpc_end_result
 */
static void
rpc_framework_get_config(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_framework_get_config_ctx req = {};   /* [한국어] 디코딩 결과를 받을 구조체 (zero-initialized — 디코딩 실패 시 free(NULL) 안전) */
	struct spdk_json_write_ctx *w;                  /* [한국어] 응답 직렬화 컨텍스트 */
	struct spdk_subsystem *subsystem;               /* [한국어] 검색된 서브시스템 객체 포인터 */

	/* [한국어] params JSON 객체를 디코더 테이블에 따라 req 구조체로 디코드 */
	if (spdk_json_decode_object(params, rpc_framework_get_config_decoders,
				    SPDK_COUNTOF(rpc_framework_get_config_decoders), &req)) {
		/* [한국어] 디코드 실패(누락/타입 오류) — 표준 INVALID_PARAMS 에러 회신 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid arguments");
		return;
	}

	/* [한국어] g_subsystems 에서 이름으로 서브시스템 검색 — 미발견이면 NULL */
	subsystem = subsystem_find(req.name);
	if (!subsystem) {                               /* [한국어] 해당 이름의 서브시스템 없음 — 사용자에게 명확한 에러 메시지 회신 */
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Subsystem '%s' not found", req.name);
		free(req.name);                         /* [한국어] strdup 으로 할당된 name 해제 — 메모리 누수 방지 */
		return;
	}

	free(req.name);                                 /* [한국어] 정상 경로에서도 name 즉시 해제 (이후 사용 없음) */

	w = spdk_jsonrpc_begin_result(request);         /* [한국어] 응답 작성 시작 */
	/* [한국어] 서브시스템의 write_config_json 콜백을 호출해 현재 활성 설정을 JSON 으로 작성.
	 * 콜백이 NULL 인 경우 subsystem_config_json 내부에서 spdk_json_write_null(w) 처리. */
	subsystem_config_json(w, subsystem);
	spdk_jsonrpc_end_result(request, w);            /* [한국어] 응답 송출 */
}

/* [한국어] framework_get_config RPC 등록 — RUNTIME phase 에서만 사용 가능. */
SPDK_RPC_REGISTER("framework_get_config", rpc_framework_get_config, SPDK_RPC_RUNTIME)

/*
 * [한국어]
 * dump_pci_device - 단일 PCI 디바이스 정보를 JSON 객체로 직렬화
 *
 * @ctx: spdk_pci_for_each_device 가 전달하는 사용자 컨텍스트. 본 콜백에서는
 *       spdk_json_write_ctx* (JSON 작성기) 가 들어온다.
 * @dev: 현재 enumerate 중인 PCI 디바이스 핸들 (env_dpdk 의 spdk_pci_device).
 * @return: void.
 *
 * 한 PCI 디바이스의 BDF 주소, 타입(드라이버 종류), config space 를 JSON 으로 작성한다.
 * config space 는 PCIe 표준상 256B 가 표준 영역, 그 뒤 4096-256=3840B 가 확장 영역이다.
 * 확장 영역이 모두 0 이면 표준 영역만 쓰고, 그렇지 않으면 전체 4KB 를 base64
 * (spdk_json_write_bytearray) 로 인코딩하여 출력한다 — 응답 크기 절감 목적.
 *
 * 호출 체인:
 *   rpc_framework_get_pci_devices → spdk_pci_for_each_device → [dump_pci_device]
 *     → spdk_pci_device_get_addr / get_type / cfg_read (lib/env_dpdk)
 */
static void
dump_pci_device(void *ctx, struct spdk_pci_device *dev)
{
	struct spdk_json_write_ctx *w = ctx;            /* [한국어] void* 콜백 컨텍스트를 JSON 작성기로 캐스팅 */
	struct spdk_pci_addr addr;                      /* [한국어] PCI BDF (domain:bus:device.function) 구조체 */
	char config[4096], bdf[32];                     /* [한국어] 4KB cfg space 버퍼 + BDF 문자열 버퍼 (예: "0000:01:00.0") */
	int rc, length = 0;                             /* [한국어] rc=cfg_read 결과, length=출력 시 실제 cfg 바이트 수 */

	addr = spdk_pci_device_get_addr(dev);           /* [한국어] 디바이스의 PCI 주소(BDF) 획득 — env_dpdk 가 DPDK rte_pci 에서 추출 */
	spdk_pci_addr_fmt(bdf, sizeof(bdf), &addr);     /* [한국어] BDF 를 표준 문자열 "DDDD:BB:DD.F" 로 포맷 */

	spdk_json_write_object_begin(w);                                          /* [한국어] 디바이스 객체 '{' 시작 */
	spdk_json_write_named_string(w, "address", bdf);                          /* [한국어] "address":"<BDF>" */
	spdk_json_write_named_string(w, "type", spdk_pci_device_get_type(dev));   /* [한국어] "type":"nvme"/"virtio" 등 — 디바이스 종류 식별자 */

	spdk_json_write_name(w, "config_space");        /* [한국어] "config_space" 키 작성 — 값은 바이트 배열로 곧 작성 */
	/* [한국어] PCI cfg space 의 처음 256B (Type 0/Type 1 표준 영역) 읽기.
	 * cfg_read 는 내부적으로 vfio/uio 의 BAR 매핑 또는 sysfs config 파일을 통해 PCIe ECAM/CAM 을 접근. */
	rc = spdk_pci_device_cfg_read(dev, config, 256, 0);
	if (rc == 0) {                                  /* [한국어] 256B 표준 영역 읽기 성공 — 일단 length=256 으로 확정 */
		length = 256;

		/* [한국어] 256B 이후의 PCIe Extended Configuration Space (오프셋 256~4095, 3840B) 읽기 시도.
		 * PCIe 디바이스만 이 영역이 의미 있으며, legacy PCI 는 0 으로 채워진다. */
		rc = spdk_pci_device_cfg_read(dev, &config[256], sizeof(config) - 256, 256);
		if (rc == 0 && spdk_mem_all_zero(&config[256], sizeof(config) - 256) != 0) {
			/* [한국어] 확장 영역이 전부 0 이 아닌 경우(=PCIe 디바이스로 의미있는 데이터 존재) 전체 4KB 출력 */
			/* Don't write the extended config space if it's all zeroes */
			length = sizeof(config);
		}
	}

	spdk_json_write_bytearray(w, config, length);   /* [한국어] cfg 버퍼를 base64 인코딩하여 JSON 값으로 작성 (length 바이트만) */
	spdk_json_write_object_end(w);                  /* [한국어] 디바이스 객체 '}' 종결 */
}

/*
 * [한국어]
 * rpc_framework_get_pci_devices - "framework_get_pci_devices" RPC 핸들러
 *
 * @request: RPC 요청 컨텍스트.
 * @params: 인자 없음 — NULL 이어야 함.
 * @return: void. 모든 PCI 디바이스 정보의 JSON 배열로 응답.
 *
 * 외부 도구의 인벤토리/디버깅 용도. spdk_pci_for_each_device 가 DPDK PCI bus
 * 에 등록된 모든 디바이스를 순회하며 dump_pci_device 콜백을 호출, 결과 JSON 배열을 회신.
 *
 * 호출 체인:
 *   jsonrpc dispatch → [rpc_framework_get_pci_devices]
 *     → spdk_pci_for_each_device (lib/env_dpdk)
 *       → dump_pci_device (위 함수) (각 PCI 디바이스마다)
 */
static void
rpc_framework_get_pci_devices(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;                  /* [한국어] 응답 작성 컨텍스트 */

	if (params != NULL) {                           /* [한국어] 인자가 있으면 거부 — 본 메서드는 무인자 */
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "framework_get_pci_devices doesn't accept any parameters.\n");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);         /* [한국어] 응답 작성 시작 */

	spdk_json_write_array_begin(w);                 /* [한국어] PCI 디바이스 배열 '[' 시작 */
	/* [한국어] DPDK PCI bus 의 등록된 모든 디바이스 enumerate.
	 * env_dpdk 가 내부 g_pci_devices 리스트를 순회하며 dump_pci_device(w, dev) 호출. */
	spdk_pci_for_each_device(w, dump_pci_device);
	spdk_json_write_array_end(w);                   /* [한국어] 배열 ']' 종결 */

	spdk_jsonrpc_end_result(request, w);            /* [한국어] 응답 송출 */
}
/* [한국어] framework_get_pci_devices RPC 등록 — RUNTIME phase 에서만 호출 가능. */
SPDK_RPC_REGISTER("framework_get_pci_devices", rpc_framework_get_pci_devices, SPDK_RPC_RUNTIME)
