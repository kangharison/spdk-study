/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

/*
 * [한국어 설명] FTL 런타임 튜너블 프로퍼티 시스템 구현 (ftl_property.c)
 *
 * === 파일의 역할 ===
 * ftl_property.h가 선언한 인터페이스의 구현. 디바이스마다 dev->properties라는 단일 LIST를 두어
 * 프로퍼티 디스크립터(struct ftl_property)들을 관리한다. 각 디스크립터는 (name, value 포인터, size,
 * unit, desc, dump/decode/set 콜백, verbose_mode)를 담는다. 외부에서 들어오는 RPC 요청은 모두
 * 이 LIST를 이름으로 검색해 처리된다. 이 파일에는 등록/해제, 이름 검색, JSON dump 빌더,
 * 일반 setter/decoder 등 정수 로직만 들어 있고 비동기 I/O는 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK JSON-RPC 처리와 FTL ftl_mngt 파이프라인의 접합점.
 * 호출 체인:
 *   - dump 경로: bdev_ftl_get_properties RPC 핸들러 → ftl_property_dump → 모든 visible 프로퍼티 dump.
 *   - set 경로: bdev_ftl_set_property → ftl_property_decode(이름→값) → ftl_property_set(콜백 디스패치) →
 *     ftl_property_set_generic 또는 사용자 정의 setter → ftl_mngt_next_step.
 * 실행 컨텍스트: SPDK reactor 스레드(JSON-RPC와 mngt 파이프라인이 같은 스레드 위에서 실행).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/queue.h(LIST_*), spdk/json.h, spdk/jsonrpc.h, ftl_core.h(spdk_ftl_dev),
 *   ftl_property.h(공개 선언), mngt/ftl_mngt.h(ftl_mngt_next_step, ftl_mngt_process).
 * 의존되는 모듈: ftl_core.c(init/deinit 호출), bdev_ftl RPC 핸들러, 프로퍼티를 등록하는 모든 모듈.
 * 데이터 흐름: 프로퍼티 메타데이터는 LIST에, 실제 값은 호출자가 제공한 외부 메모리(dev 안의 필드)에.
 *   JSON-RPC 응답은 SPDK json write API로 직접 직렬화되어 소켓으로 흐름.
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct ftl_properties: dev->properties — LIST head 한 개.
 *   - struct ftl_property: 프로퍼티 디스크립터 — 모든 RPC 메타데이터와 콜백 보유.
 *   - get_property: static — 이름으로 LIST 선형 검색.
 *   - ftl_property_register: 새 프로퍼티 추가(중복 시 abort).
 *   - ftl_properties_init/deinit: dev 초기화/정리.
 *   - is_property_visible: static — verbose 모드 가시성 판정.
 *   - ftl_property_dump_common_begin/end: static — 모든 dump가 공통으로 출력하는 메타데이터.
 *   - ftl_property_dump: 전체 dump 디스패처.
 *   - ftl_property_dump_bool/uint64/uint32: 타입별 value dump.
 *   - ftl_property_decode: 이름 → decode 콜백 디스패처.
 *   - ftl_property_set: 이름 → set 콜백 디스패처.
 *   - ftl_property_set_generic: memcpy + next_step.
 *   - ftl_property_decode_bool: "true"/"false" → bool 디코더.
 */

#include "spdk/queue.h"
/* [한국어] LIST_HEAD/LIST_ENTRY/LIST_FOREACH 등 BSD-style 단일 연결 리스트 매크로. */
#include "spdk/json.h"
/* [한국어] spdk_json_write_named_string/bool/uint64/uint32 — JSON 출력 API. */
#include "spdk/jsonrpc.h"
/* [한국어] spdk_jsonrpc_begin_result/end_result — RPC 응답 라이프사이클. */

#include "ftl_core.h"
/* [한국어] struct spdk_ftl_dev (properties 멤버, conf.name, conf.verbose_mode 접근), FTL_ERRLOG 매크로. */
#include "ftl_property.h"
/* [한국어] 공개 인터페이스 선언 — 구현 일치 검증 위해 포함. */
#include "mngt/ftl_mngt.h"
/* [한국어] ftl_mngt_process, ftl_mngt_next_step — set 경로에서 mngt 파이프라인 진행 신호. */

struct ftl_properties {
	LIST_HEAD(, ftl_property) list;
	/* [한국어] 등록된 ftl_property 디스크립터들의 LIST head.
	 * 설정자: ftl_properties_init이 LIST_INIT으로 초기화, ftl_property_register가 LIST_INSERT_HEAD.
	 * 읽는 자: get_property/dump가 LIST_FOREACH로 순회.
	 * 동기화: 모든 RPC 핸들러와 mngt가 같은 reactor 스레드에서 실행되므로 락 불필요. */
};

/**
 * @brief FTL property descriptor
 */
struct ftl_property {
	/** Name of the property */
	const char *name;
	/* [한국어] 프로퍼티 이름 — 등록 시점에 호출자가 제공한 const char* 그대로 보관(복제 안 함).
	 * 설정자: ftl_property_register. 읽는 자: get_property strcmp 매칭, dump 출력.
	 * 값 범위: NUL 종료 문자열, 등록 후 라이프타임 유지 필수(주로 컴파일 상수). */

	/* Pointer to the value of property */
	void *value;
	/* [한국어] 실제 값을 가리키는 포인터 — dev 내부의 어떤 필드 또는 별도 메모리.
	 * 설정자: register. 읽는 자: dump_bool/uint64/uint32가 역참조로 값 읽기.
	 * 동기화: read-only로 dump, set_generic이 memcpy로 갱신 — 같은 reactor 위에서 직렬화됨. */

	/* The value size of the property */
	size_t size;
	/* [한국어] 값의 바이트 크기. dump/decode/set 모두 size와 일치 검증 후 동작.
	 * 설정자: register. 읽는 자: dump_bool 등의 어서션, set_generic의 ftl_bug 검사. */

	/** The unit of the property value */
	const char *unit;
	/* [한국어] 단위 문자열("ms", "GiB", NULL 가능). NULL이면 dump에 unit 미출력.
	 * 설정자: register. 읽는 자: ftl_property_dump_common_end. */

	/** The property description for user help */
	const char *desc;
	/* [한국어] 사용자 도움말 문자열(NULL 가능). 마찬가지로 dump_common_end에서 출력.
	 * 설정자: register. */

	/* The function to dump the value of property into the specified JSON RPC request */
	ftl_property_dump_fn dump;
	/* [한국어] dump 콜백 — JSON에 "value": ... 라인 출력 책임.
	 * 설정자: register. 읽는 자: ftl_property_dump가 모든 visible 프로퍼티에 대해 호출. */

	/* Decode property value and store it in output */
	ftl_property_decode_fn decode;
	/* [한국어] decode 콜백(NULL이면 read-only 표시).
	 * 설정자: register. 읽는 자: ftl_property_decode가 호출. */

	/* Set the FTL property */
	ftl_property_set_fn set;
	/* [한국어] set 콜백(NULL이면 read-only 표시).
	 * 설정자: register. 읽는 자: ftl_property_set이 호출. mngt next_step을 콜백 안에서 호출 의무. */

	/* It indicates the property is available in verbose mode only */
	bool verbose_mode;
	/* [한국어] verbose 모드 한정 가시성 플래그.
	 * 설정자: register. 읽는 자: is_property_visible. 값 범위: true/false. */

	/** Link to put the property to the list */
	LIST_ENTRY(ftl_property) entry;
	/* [한국어] LIST 노드 링크 — 다음 노드 포인터.
	 * 설정자/읽는 자: LIST_INSERT_HEAD/REMOVE/FOREACH 매크로 내부에서만 다룬다. */
};

/*
 * [한국어]
 * get_property - 이름으로 dev->properties LIST 선형 검색
 *
 * @param properties: dev->properties (LIST head 포함 구조체).
 * @param name: 찾을 프로퍼티 이름.
 * @return: 매칭된 ftl_property* 또는 NULL.
 *
 * 동작: LIST_FOREACH로 모든 노드 순회 → strcmp로 이름 비교.
 * 실행 컨텍스트: SPDK reactor 스레드. 동기화 불필요(같은 스레드 직렬화).
 * 복잡도: O(N) — 프로퍼티 수가 적어 충분히 허용 가능.
 *
 * 호출 체인:
 *   ftl_property_register / decode / set → [이 함수]
 */
static struct ftl_property *
get_property(struct ftl_properties *properties, const char *name)
{
	struct ftl_property *entry;
	/* [한국어] LIST_FOREACH 반복 변수. */

	LIST_FOREACH(entry, &properties->list, entry) {
		/* [한국어] LIST head를 시작점으로 모든 노드를 entry 멤버로 따라 순회.
		 * 두 번째 'entry'는 struct ftl_property 안의 LIST_ENTRY 멤버 이름. */
		/* TODO think about strncmp */
		if (0 == strcmp(entry->name, name)) {
			/* [한국어] 이름 정확 매칭 — TODO 코멘트는 입력 길이 제한이 필요한지 추후 검토를 의미.
			 * 현재는 등록 시 const 문자열만 받으므로 strcmp로 충분. */
			return entry;
			/* [한국어] 첫 매칭 즉시 반환 — 이름 중복은 register에서 차단되어 단 하나만 존재. */
		}
	}

	return NULL;
	/* [한국어] 못 찾음. 호출자(decode/set)는 -ENOENT로 변환. */
}

/*
 * [한국어]
 * ftl_property_register - 새 프로퍼티 디스크립터를 LIST에 추가
 *
 * 자세한 시맨틱은 ftl_property.h 함수 주석 참조.
 *
 * 동작 단계:
 *   1) get_property로 동일 이름 중복 여부 검사 — 중복이면 ftl_abort()로 즉시 종료(코딩 오류).
 *   2) 디스크립터 calloc — OOM이면 ftl_abort().
 *   3) 모든 필드 채움.
 *   4) LIST_INSERT_HEAD로 LIST 앞쪽에 삽입(O(1)).
 *
 * 실행 컨텍스트: 디바이스 init mngt 단계, SPDK reactor 스레드, 단일 스레드 호출.
 *
 * 호출 체인:
 *   ftl_mngt_init_properties / 모듈별 init → [이 함수] → calloc/LIST_INSERT_HEAD
 */
void
ftl_property_register(struct spdk_ftl_dev *dev,
		      const char *name, void *value, size_t size,
		      const char *unit, const char *desc,
		      ftl_property_dump_fn dump,
		      ftl_property_decode_fn decode,
		      ftl_property_set_fn set,
		      bool verbose_mode)
{
	struct ftl_properties *properties = dev->properties;
	/* [한국어] dev->properties 단축 — ftl_properties_init이 미리 할당해 둠. */

	if (get_property(properties, name)) {
		/* [한국어] 동일 이름이 이미 등록됨 — 코딩 오류이므로 즉시 abort.
		 * 마운트가 진행되어 봤자 데이터 일관성이 깨지므로 프로세스 종료가 안전. */
		FTL_ERRLOG(dev, "FTL property registration ERROR, already exist, name %s\n", name);
		ftl_abort();
		/* [한국어] assert(false) + abort() — SIGABRT로 프로세스 종료. */
	} else {
		struct ftl_property *prop = calloc(1, sizeof(*prop));
		/* [한국어] 디스크립터 한 개 할당 — entry 링크 포함 약 80바이트 정도. */
		if (NULL == prop) {
			/* [한국어] OOM — 마찬가지로 복구 불가. */
			FTL_ERRLOG(dev, "FTL property registration ERROR, out of memory, name %s\n", name);
			ftl_abort();
		}

		prop->name = name;
		/* [한국어] 호출자가 제공한 const 문자열 포인터를 그대로 보관(복제 안 함 — 라이프타임 유지 가정). */
		prop->value = value;
		/* [한국어] dev 내부 필드 등 외부 메모리를 가리키는 포인터. */
		prop->size = size;
		/* [한국어] 값 크기. */
		prop->unit = unit;
		/* [한국어] 단위 문자열. NULL 가능. */
		prop->desc = desc;
		/* [한국어] 도움말 문자열. NULL 가능. */
		prop->dump = dump;
		/* [한국어] dump 콜백 — 보통 ftl_property_dump_bool/uint32/uint64 중 하나. */
		prop->decode = decode;
		/* [한국어] decode 콜백 — NULL이면 read-only로 표시되어 set 불가. */
		prop->set = set;
		/* [한국어] set 콜백 — NULL이면 read-only로 표시. */
		prop->verbose_mode = verbose_mode;
		/* [한국어] verbose 한정 가시성 플래그. */
		LIST_INSERT_HEAD(&properties->list, prop, entry);
		/* [한국어] LIST 머리에 삽입 — O(1). 순서는 의미 없음(검색은 strcmp로). */
	}
}

/*
 * [한국어]
 * ftl_properties_init - dev->properties 시스템 초기화
 *
 * @param dev: FTL 디바이스.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 동작: ftl_properties 구조체 calloc → LIST_INIT으로 빈 LIST 만들기.
 * 실행 컨텍스트: 디바이스 init mngt 단계 초기.
 */
int
ftl_properties_init(struct spdk_ftl_dev *dev)
{
	dev->properties = calloc(1, sizeof(*dev->properties));
	/* [한국어] dev->properties는 LIST head만 있는 작은 구조체. calloc으로 0 초기화. */
	if (!dev->properties) {
		/* [한국어] OOM — 호출자가 마운트 실패 처리. */
		return -ENOMEM;
	}

	LIST_INIT(&dev->properties->list);
	/* [한국어] BSD-style LIST 헤드 초기화 — 사실상 head->lh_first = NULL. */
	return 0;
	/* [한국어] 성공. 이후 register 호출 가능. */
}

/*
 * [한국어]
 * ftl_properties_deinit - 모든 프로퍼티 노드를 free하고 properties 자체도 free
 *
 * @param dev: FTL 디바이스.
 *
 * 동작: 빈 LIST가 될 때까지 LIST_FIRST → LIST_REMOVE → free 반복.
 *   마지막에 dev->properties 자체 free.
 * NULL safety: properties == NULL이면 즉시 반환(중복 deinit 방지).
 *
 * 실행 컨텍스트: 디바이스 정지/해제 mngt 단계.
 */
void
ftl_properties_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_properties *properties = dev->properties;
	/* [한국어] 단축 변수. */
	struct ftl_property *prop;
	/* [한국어] 순회 시 임시 노드 포인터. */

	if (!properties) {
		/* [한국어] init이 실패했거나 이미 deinit 되었으면 무시(idempotent). */
		return;
	}

	while (!LIST_EMPTY(&properties->list)) {
		/* [한국어] LIST가 빌 때까지 반복 — LIST 길이만큼 free 횟수. */
		prop = LIST_FIRST(&properties->list);
		/* [한국어] LIST 머리 노드 추출. */
		LIST_REMOVE(prop, entry);
		/* [한국어] LIST에서 노드 제거(O(1)). entry 링크가 자동 정리됨. */
		free(prop);
		/* [한국어] 디스크립터 메모리 free. value 포인터가 가리키는 외부 메모리는 dev 소유라 건드리지 않음. */
	}

	free(dev->properties);
	/* [한국어] LIST head 구조체 자체도 free. dev->properties는 이후 NULL로 두지 않지만,
	 * 호출자(ftl_dev free 경로)가 dev 전체를 곧 해제하므로 use-after-free 위험은 없다. */
}

/*
 * [한국어]
 * is_property_visible - verbose 모드 판정에 따라 프로퍼티 가시성 결정
 *
 * @param dev: FTL 디바이스 — dev->conf.verbose_mode 검사.
 * @param prop: 검사 대상 프로퍼티.
 * @return: true = 노출, false = 숨김.
 *
 * 정책: prop->verbose_mode == true이고 dev->conf.verbose_mode == false인 경우만 숨김.
 *   그 외에는 모두 노출. 즉 verbose 한정 프로퍼티는 verbose 활성 시에만 dump/decode/set 가능.
 */
static bool
is_property_visible(struct spdk_ftl_dev *dev, struct ftl_property *prop)
{
	if (prop->verbose_mode && !dev->conf.verbose_mode) {
		/* [한국어] verbose 한정 프로퍼티 + 비-verbose 디바이스 → 숨김. */
		return false;
	}

	return true;
	/* [한국어] 그 외에는 노출. */
}

/*
 * [한국어]
 * ftl_property_dump_common_begin - dump 시작 — 모든 프로퍼티 공통으로 "name": ... 출력
 *
 * @param property: 대상 프로퍼티.
 * @param w: JSON write 컨텍스트.
 *
 * 동작: spdk_json_write_named_string으로 "name" 필드 한 줄 출력.
 * 호출자: ftl_property_dump가 각 프로퍼티마다 호출.
 */
static void
ftl_property_dump_common_begin(const struct ftl_property *property,
			       struct spdk_json_write_ctx *w)
{
	spdk_json_write_named_string(w, "name", property->name);
	/* [한국어] JSON에 {"name": "<프로퍼티 이름>"} 한 줄 출력. */
}

/*
 * [한국어]
 * ftl_property_dump_common_end - dump 종료 직전 — unit/desc/read-only 메타데이터 출력
 *
 * @param property: 대상 프로퍼티.
 * @param w: JSON write 컨텍스트.
 *
 * 동작:
 *   - unit이 NULL이 아니면 "unit": ... 출력.
 *   - desc가 NULL이 아니면 "desc": ... 출력.
 *   - decode 또는 set 콜백이 NULL이면 "read-only": true 출력.
 */
static void
ftl_property_dump_common_end(const struct ftl_property *property,
			     struct spdk_json_write_ctx *w)
{
	if (property->unit) {
		/* [한국어] 단위가 등록된 경우만 출력 — JSON 응답에서 빈 필드 회피. */
		spdk_json_write_named_string(w, "unit", property->unit);
	}
	if (property->desc) {
		/* [한국어] 도움말이 있는 경우만 출력. */
		spdk_json_write_named_string(w, "desc", property->desc);
	}

	if (!property->decode || !property->set) {
		/* [한국어] decode 또는 set 둘 중 하나라도 NULL이면 사실상 read-only.
		 * 사용자가 클라이언트 측에서 set RPC를 보내봤자 거부될 것이므로 미리 표시. */
		spdk_json_write_named_bool(w, "read-only", true);
	}
}

/*
 * [한국어]
 * ftl_property_dump - dev의 모든 visible 프로퍼티를 JSON-RPC 응답으로 dump
 *
 * 자세한 시맨틱은 ftl_property.h 참조.
 *
 * 동작 단계:
 *   1) spdk_jsonrpc_begin_result로 응답 시작 — write 컨텍스트 획득.
 *   2) 응답 객체 시작 → "name": dev name 출력.
 *   3) "properties" 배열 시작 → 모든 프로퍼티 LIST 순회:
 *      - is_property_visible 검사로 숨겨진 항목 건너뜀.
 *      - 각 항목을 객체로 begin → common_begin → 타입별 dump → common_end → object_end.
 *   4) 배열/객체 종료 → spdk_jsonrpc_end_result로 응답 송신.
 *
 * 실행 컨텍스트: SPDK reactor 스레드(JSON-RPC 핸들러).
 */
void
ftl_property_dump(struct spdk_ftl_dev *dev, struct spdk_jsonrpc_request *request)
{
	struct ftl_properties *properties = dev->properties;
	/* [한국어] 단축 변수. */
	struct ftl_property *prop;
	/* [한국어] LIST 순회 변수. */
	struct spdk_json_write_ctx *w;
	/* [한국어] JSON write 컨텍스트 — begin_result로 획득. */

	w = spdk_jsonrpc_begin_result(request);
	/* [한국어] RPC 응답 시작 — 이후 spdk_json_write_* 호출이 응답 buf로 누적되고 end_result에서 송신. */

	spdk_json_write_object_begin(w);
	/* [한국어] 응답 최상위 JSON 객체 시작 '{'. */
	spdk_json_write_named_string(w, "name", dev->conf.name);
	/* [한국어] 디바이스 이름을 응답에 포함 — 사용자가 어느 FTL 인스턴스의 응답인지 식별 가능. */

	spdk_json_write_named_array_begin(w, "properties");
	/* [한국어] "properties": [ — 배열 시작. */
	LIST_FOREACH(prop, &properties->list, entry) {
		/* [한국어] 모든 프로퍼티 순회. */
		if (!is_property_visible(dev, prop)) {
			/* [한국어] verbose 한정 + 비-verbose 디바이스 → 건너뜀. */
			continue;
		}

		spdk_json_write_object_begin(w);
		/* [한국어] 한 프로퍼티를 표현할 JSON 객체 '{' 시작. */
		ftl_property_dump_common_begin(prop, w);
		/* [한국어] "name": ... 출력. */
		prop->dump(dev, prop, w);
		/* [한국어] 타입별 dump 콜백 — "value": ... 출력. */
		ftl_property_dump_common_end(prop, w);
		/* [한국어] "unit"/"desc"/"read-only": ... 출력. */
		spdk_json_write_object_end(w);
		/* [한국어] '}' 종료. */
	}
	spdk_json_write_array_end(w);
	/* [한국어] ']' 배열 종료. */

	spdk_json_write_object_end(w);
	/* [한국어] 응답 최상위 객체 '}' 종료. */
	spdk_jsonrpc_end_result(request, w);
	/* [한국어] 응답 송신 — 누적된 JSON이 클라이언트로 전송. */
}

/*
 * [한국어]
 * ftl_property_dump_bool - bool 프로퍼티의 값을 JSON named bool로 출력
 *
 * @param dev: 미사용. @param property: 대상. @param w: JSON 컨텍스트.
 *
 * 동작: property->size == sizeof(bool) 어서션 후 *(bool*)value를 "value": true/false로 출력.
 */
void
ftl_property_dump_bool(struct spdk_ftl_dev *dev, const struct ftl_property *property,
		       struct spdk_json_write_ctx *w)
{
	bool *value = property->value;
	/* [한국어] value 포인터를 bool*로 캐스팅 — 등록 시 size 검증으로 안전 보장. */

	assert(property->size == sizeof(*value));
	/* [한국어] 디버그 빌드에서 size 일치 검증 — 잘못된 등록 시 즉시 잡음. */
	spdk_json_write_named_bool(w, "value", *value);
	/* [한국어] "value": true/false 출력. */
}

/*
 * [한국어]
 * ftl_property_dump_uint64 - uint64_t 프로퍼티 값을 JSON named uint64로 출력
 */
void
ftl_property_dump_uint64(struct spdk_ftl_dev *dev, const struct ftl_property *property,
			 struct spdk_json_write_ctx *w)
{
	uint64_t *value = property->value;
	/* [한국어] uint64_t* 캐스팅. */

	assert(property->size == sizeof(*value));
	/* [한국어] 8바이트 일치 검증. */
	spdk_json_write_named_uint64(w, "value", *value);
	/* [한국어] JSON으로 8바이트 양의 정수 출력. */
}

/*
 * [한국어]
 * ftl_property_dump_uint32 - uint32_t 프로퍼티 값을 JSON named uint32로 출력
 */
void
ftl_property_dump_uint32(struct spdk_ftl_dev *dev, const struct ftl_property *property,
			 struct spdk_json_write_ctx *w)
{
	uint32_t *value = property->value;
	/* [한국어] uint32_t* 캐스팅. */

	assert(property->size == sizeof(*value));
	/* [한국어] 4바이트 일치 검증. */
	spdk_json_write_named_uint32(w, "value", *value);
	/* [한국어] JSON 4바이트 양의 정수 출력. */
}

/*
 * [한국어]
 * ftl_property_decode - 이름으로 프로퍼티 찾고 decode 콜백을 호출해 새 값을 binary로 변환
 *
 * 자세한 시맨틱은 ftl_property.h 참조.
 *
 * 동작:
 *   1) get_property로 매칭 — 없으면 -ENOENT.
 *   2) decode 콜백 NULL인지 검사 — read-only면 -EACCES.
 *   3) 가시성 검사 — verbose 한정 미노출이면 -EACCES.
 *   4) prop->size 만큼 *output 버퍼 calloc.
 *   5) decode 콜백 호출 — 실패 시 *output free 후 음수 코드 반환.
 *
 * 호출자 의무: 성공 시 *output을 free 책임. *output은 호출 전 NULL이어야 함(어서션).
 */
int
ftl_property_decode(struct spdk_ftl_dev *dev, const char *name, const char *value,
		    size_t value_size, void **output, size_t *output_size)
{
	struct ftl_properties *properties = dev->properties;
	/* [한국어] 단축 변수. */
	struct ftl_property *prop = get_property(properties, name);
	/* [한국어] 이름으로 매칭. NULL이면 미존재. */
	int rc;
	/* [한국어] decode 콜백 반환값 임시 저장. */

	if (!prop) {
		/* [한국어] 이름이 LIST에 없음. */
		FTL_ERRLOG(dev, "Property doesn't exist, name %s\n", name);
		return -ENOENT;
	}

	if (!prop->decode) {
		/* [한국어] decode 콜백 미등록 → 사용자가 set 시도 불가. */
		FTL_ERRLOG(dev, "Property is read only, name %s\n", name);
		return -EACCES;
	}

	if (!is_property_visible(dev, prop)) {
		/* [한국어] verbose 한정 미노출 — verbose 켜라는 메시지로 사용자 안내. */
		FTL_ERRLOG(dev, "Property is inactive, enable verbose mode to access it, name %s.\n", name);
		return -EACCES;
	}

	assert(prop->size);
	/* [한국어] 0 크기는 등록 시점에 거부되었어야 함 — 안전망. */
	assert(NULL == *output);
	/* [한국어] 호출자는 *output을 NULL로 초기화해 넘겨야 함 — 누수 방지. */

	/* Allocate buffer for the new value of the property */
	*output = calloc(1, prop->size);
	/* [한국어] 새 값을 담을 버퍼 prop->size 바이트 할당 — 0 초기화. */
	if (NULL == *output) {
		/* [한국어] OOM. */
		FTL_ERRLOG(dev, "Property allocation memory error, name %s\n", name);
		return -EACCES;
		/* [한국어] -EACCES — 의미상 -ENOMEM이 더 정확하지만 원본 코드 유지. */
	}
	*output_size = prop->size;
	/* [한국어] 호출자에게 버퍼 크기 통보. */

	rc = prop->decode(dev, prop, value, value_size, *output, *output_size);
	/* [한국어] 등록된 decode 콜백 호출 — bool 디코더는 "true"/"false" 매칭, 다른 타입은 별도 구현. */
	if (rc) {
		/* [한국어] 디코드 실패 — 사용자 입력이 잘못된 형식. */
		FTL_ERRLOG(dev, "Property decode error, name %s\n", name);
		free(*output);
		/* [한국어] 누수 방지 — 실패 시 호출자가 free 책임을 가지지 않도록 미리 정리. */
		*output = NULL;
		/* [한국어] dangling 포인터 회피 — 호출자가 NULL 검사로 실패 인식 가능. */
		return rc;
	}

	return 0;
	/* [한국어] 성공. 호출자는 ftl_property_set으로 적용 후 *output을 free해야 함. */
}

/*
 * [한국어]
 * ftl_property_set - 디코드된 값을 mngt 통해 안전 적용
 *
 * 자세한 시맨틱은 ftl_property.h 참조.
 *
 * 동작:
 *   1) get_property로 매칭 — 없으면 -ENOENT.
 *   2) set 콜백 NULL이면 -EACCES.
 *   3) 가시성 검사 — 미노출이면 -EACCES.
 *   4) set 콜백 호출 — 콜백이 mngt next_step을 호출해 다음 단계 진행.
 *
 * 콜백 자체는 비동기일 수도 있으므로 이 함수가 0을 반환했다고 해서 적용이 끝났다는 보장은 아니다.
 */
int
ftl_property_set(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
		 const char *name, void *value, size_t value_size)
{
	struct ftl_properties *properties = dev->properties;
	/* [한국어] 단축 변수. */
	struct ftl_property *prop = get_property(properties, name);
	/* [한국어] 이름 매칭. */

	if (!prop) {
		/* [한국어] 이름 미존재 — 데코드 단계에서 잡혔어야 하지만 안전망. */
		FTL_ERRLOG(dev, "Property doesn't exist, name %s\n", name);
		return -ENOENT;
	}

	if (!prop->set) {
		/* [한국어] read-only. */
		FTL_ERRLOG(dev, "Property is read only, name %s\n", name);
		return -EACCES;
	}

	if (!is_property_visible(dev, prop)) {
		/* [한국어] verbose 한정 미노출. */
		FTL_ERRLOG(dev, "Property is inactive, enable verbose mode to access it, name %s\n", name);
		return -EACCES;
	}

	prop->set(dev, mngt, prop, value, value_size);
	/* [한국어] set 콜백 호출 — 콜백이 직접 값 갱신 + ftl_mngt_next_step(mngt) 호출 의무.
	 * 호출 후 콜백이 동기/비동기 어느 쪽이든 이 함수는 0 반환. */
	return 0;
}

/*
 * [한국어]
 * ftl_property_set_generic - memcpy 기반 표준 setter — 대부분의 프로퍼티가 사용
 *
 * @param dev: 미사용. @param mngt: mngt 핸들. @param property: 대상. @param new_value/new_value_size: 새 값.
 *
 * 동작: size 일치 검증(ftl_bug — 불일치 시 abort) → memcpy → mngt next_step 호출.
 * 사용 예: ftl_property_register_bool_rw가 자동으로 이 setter를 등록.
 */
void
ftl_property_set_generic(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
			 const struct ftl_property *property, void *new_value, size_t new_value_size)
{
	ftl_bug(property->size != new_value_size);
	/* [한국어] size 불일치는 코딩 오류 — 즉시 abort. spdk_unlikely로 분기 예측 최적화. */
	memcpy(property->value, new_value, property->size);
	/* [한국어] 외부 메모리에 binary 복사 — value 포인터가 가리키는 dev 내부 필드가 갱신됨. */
	ftl_mngt_next_step(mngt);
	/* [한국어] mngt 파이프라인에 다음 단계 진행 신호 — set RPC가 동기적으로 완료된 것처럼 처리. */
}

/*
 * [한국어]
 * ftl_property_decode_bool - "true"/"false" 문자열을 bool로 디코드
 *
 * @param dev/property: 미사용.
 * @param value/value_size: 입력 문자열과 길이.
 * @param output: bool* 결과 저장 포인터.
 * @param output_size: sizeof(bool)이어야 함.
 * @return: 0 성공, -ENOBUFS 크기 불일치, -EINVAL 매칭 실패/비-NUL 종료.
 *
 * 동작:
 *   1) output_size != sizeof(bool)이면 -ENOBUFS.
 *   2) value가 NUL 종료가 안 됐으면 -EINVAL.
 *   3) "true"이면 *out = true 후 0.
 *   4) "false"이면 *out = false 후 0.
 *   5) 그 외 -EINVAL.
 */
int
ftl_property_decode_bool(struct spdk_ftl_dev *dev, struct ftl_property *property,
			 const char *value, size_t value_size, void *output, size_t output_size)
{
	bool *out = output;
	/* [한국어] output을 bool*로 캐스팅 — 호출자가 sizeof(bool) 버퍼 보장. */

	if (sizeof(bool) != output_size) {
		/* [한국어] 호출자가 잘못된 크기 버퍼를 넘김 — 디코드 거부. */
		return -ENOBUFS;
	}

	if (strnlen(value, value_size) == value_size) {
		/* [한국어] strnlen이 value_size를 반환 = 버퍼 안에 NUL 없음 = 비정상 입력.
		 * 이후 strncmp가 정의되지 않은 동작을 일으키지 않도록 사전 차단. */
		return -EINVAL;
	}

	if (strncmp(value, "true", spdk_max(strlen("true"), value_size)) == 0) {
		/* [한국어] "true"와 일치(spdk_max로 더 긴 쪽 길이 사용 — value가 4자보다 짧아도 안전 비교). */
		*out = true;
		/* [한국어] 결과 저장. */
		return 0;
		/* [한국어] 성공. */
	}

	if (strncmp(value, "false", spdk_max(strlen("false"), value_size)) == 0) {
		/* [한국어] "false"와 일치. */
		*out = false;
		return 0;
	}

	return -EINVAL;
	/* [한국어] 어떤 매칭에도 해당 안 됨 — 잘못된 입력. */
}
