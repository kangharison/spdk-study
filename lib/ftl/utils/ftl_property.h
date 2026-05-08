/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

/*
 * [한국어 설명] FTL 런타임 튜너블 프로퍼티 시스템 공개 인터페이스 (ftl_property.h)
 *
 * === 파일의 역할 ===
 * 디바이스 생성 시점에 등록된 프로퍼티들을 JSON-RPC로 조회/수정할 수 있게 해주는 시스템의 공개
 * 인터페이스. 각 프로퍼티는 (이름, 값 포인터, 크기, 단위, 설명, dump/decode/set 콜백, verbose 모드 플래그)
 * 의 튜플로 등록된다. 사용자는 bdev_ftl_get_properties RPC로 모두 조회하거나
 * bdev_ftl_set_property RPC로 특정 프로퍼티 값을 갱신할 수 있다. 타입별 헬퍼(bool/uint32/uint64
 * dump 함수, bool decoder, generic setter)와 read-write bool 등록을 한 줄로 처리하는 인라인 wrapper도 제공.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK JSON-RPC 시스템과 FTL 런타임 튜닝 사이의 다리.
 * 호출 체인: 사용자 → spdk_jsonrpc 핸들러(bdev_ftl_set_property) → ftl_property_decode →
 *   ftl_property_set → mngt_process를 통해 안전한 시점에 콜백 실행 → 값 반영.
 *   조회 경로: 사용자 → bdev_ftl_get_properties → ftl_property_dump → 모든 등록 프로퍼티 dump.
 * 실행 컨텍스트: SPDK reactor 스레드(JSON-RPC 처리 스레드와 동일한 reactor).
 *   set 경로는 ftl_mngt 파이프라인을 통해 비동기로 다음 단계 콜백 호출.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: spdk/stdinc.h(uint64_t/size_t/bool), 묵시적으로 spdk_jsonrpc/spdk/json
 *   (구현 시 사용), ftl_mngt(set 시점 mngt 파이프라인 next_step 호출).
 * 의존되는 모듈: ftl_property.c(구현), ftl_core.c(생성 시 dev->properties 초기화),
 *   ftl_nv_cache.c 등(verbose mode/feature flag 등록).
 * 데이터 흐름: 등록 시점에 프로퍼티 메타데이터가 dev->properties LIST에 저장 → dump/decode/set 시점에 순회/매칭.
 *
 * === 주요 함수/구조체 요약 ===
 *   - ftl_properties_init/deinit: dev 생성/해제 시 프로퍼티 시스템 초기화/정리.
 *   - ftl_property_register: 새 프로퍼티 등록(dump/decode/set 콜백 함께).
 *   - ftl_property_dump_bool/uint64/uint32: 타입별 dump 콜백 구현 — 외부에서 그대로 register에 넘기면 됨.
 *   - ftl_property_dump: dev의 모든 visible 프로퍼티를 RPC 응답으로 dump.
 *   - ftl_property_decode: 이름으로 프로퍼티 찾고 decode 콜백으로 새 값 디시리얼라이즈.
 *   - ftl_property_decode_bool: bool 디코더("true"/"false" 문자열).
 *   - ftl_property_set: decoded 새 값을 mngt 통해 안전 적용.
 *   - ftl_property_set_generic: memcpy 기반 단순 setter — 대부분의 프로퍼티에 사용.
 *   - ftl_property_register_bool_rw: read-write bool 등록 한 줄 wrapper.
 *   - 콜백 타입: ftl_property_dump_fn / ftl_property_decode_fn / ftl_property_set_fn.
 */

#ifndef FTL_PROPERTY_H
#define FTL_PROPERTY_H
/* [한국어] 헤더 가드 — 다중 포함 방지. */

#include "spdk/stdinc.h"
/* [한국어] 표준 타입 (uint64_t, size_t, bool 등). */

struct spdk_ftl_dev;
/* [한국어] forward declaration — ftl_core.h가 정의하는 메인 디바이스 컨텍스트. */
struct ftl_property;
/* [한국어] forward declaration — ftl_property.c가 정의하는 프로퍼티 디스크립터.
 * 외부에서는 const 포인터로만 다루므로 내부 필드 변경 자유. */

/**
 * @brief Init the FTL properties system
 *
 * @retval 0 Success
 * @retval Non-zero a Failure
 */
/*
 * [한국어]
 * ftl_properties_init - dev->properties 시스템 초기화 (빈 LIST 생성)
 *
 * @param dev: FTL 디바이스 — 이 함수가 dev->properties 멤버를 calloc.
 * @return: 0 = 성공, -ENOMEM 등 음수 = 실패.
 *
 * 동기/배경: 디바이스 생성 mngt 단계 초기에 호출되어 이후 ftl_property_register 호출이 가능해진다.
 * 실행 컨텍스트: SPDK reactor 스레드, 단일 호출.
 */
int ftl_properties_init(struct spdk_ftl_dev *dev);

/**
 * @brief Deinit the FTL properties system
 */
/*
 * [한국어]
 * ftl_properties_deinit - 등록된 모든 프로퍼티 해제 후 dev->properties free
 *
 * @param dev: FTL 디바이스.
 *
 * 동작: LIST의 모든 프로퍼티 노드를 순회하며 free 후 properties 자체 free. dev->properties == NULL이면 무시.
 */
void ftl_properties_deinit(struct spdk_ftl_dev *dev);

/**
 * @brief A function to dump the FTL property which type is bool
 */
/*
 * [한국어]
 * ftl_property_dump_bool - bool 타입 프로퍼티 값을 JSON에 "value": true/false 로 dump
 *
 * @param dev: FTL 디바이스(미사용 — 시그니처 호환).
 * @param property: 대상 프로퍼티 — value/size 멤버 사용.
 * @param w: SPDK JSON write 컨텍스트 — 출력 대상.
 *
 * 동작: property->size == sizeof(bool) 어서션 후 *(bool*)property->value를 JSON named bool로 출력.
 */
void ftl_property_dump_bool(struct spdk_ftl_dev *dev, const struct ftl_property *property,
			    struct spdk_json_write_ctx *w);

/**
 * @brief A function to dump the FTL property which type is uint64
 */
/*
 * [한국어]
 * ftl_property_dump_uint64 - uint64_t 프로퍼티 값을 JSON named uint64로 dump
 *
 * @param dev/property/w: 위와 동일.
 */
void ftl_property_dump_uint64(struct spdk_ftl_dev *dev, const struct ftl_property *property,
			      struct spdk_json_write_ctx *w);

/**
 * @brief A function to dump the FTL property which type is uint32
 */
/*
 * [한국어]
 * ftl_property_dump_uint32 - uint32_t 프로퍼티 값을 JSON named uint32로 dump
 *
 * @param dev/property/w: 위와 동일.
 */
void ftl_property_dump_uint32(struct spdk_ftl_dev *dev, const struct ftl_property *property,
			      struct spdk_json_write_ctx *w);

/**
 * @brief Dump the value of property into the specified JSON RPC request
 *
 * @param dev FTL device
 * @param property The property to dump to the JSON RPC request
 * @param[out] w JSON RPC request
 */
typedef void (*ftl_property_dump_fn)(struct spdk_ftl_dev *dev, const struct ftl_property *property,
				     struct spdk_json_write_ctx *w);
/* [한국어] dump 콜백 타입 — 등록 시점에 type별 dump 함수(예: ftl_property_dump_bool)를 넘긴다.
 * SPDK JSON write API로 "value": ... 출력 책임. */

/**
 * @brief Decode property value and store it in output
 *
 * @param dev FTL device
 * @param property The property
 * @param value The new property value
 * @param value_size The size of the value buffer
 * @param output The output where to store new value
 * @param output_size The decoded value output size
 */
typedef int (*ftl_property_decode_fn)(struct spdk_ftl_dev *dev, struct ftl_property *property,
				      const char *value, size_t value_size, void *output, size_t output_size);
/* [한국어] decode 콜백 타입 — 사용자 입력 문자열(value, value_size)을 binary 형태로 변환해 output(output_size)에 저장.
 * 반환값: 0 성공, 음수(-EINVAL/-ENOBUFS 등) 실패. NULL이면 read-only 프로퍼티. */

/**
 * @brief Set the FTL property
 *
 * @param dev FTL device
 * @param mngt FTL management process handle
 * @param property The property
 * @param new_value The new property value to be set
 * @param new_value_size The size of the new property value
 */
typedef void (*ftl_property_set_fn)(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
				    const struct ftl_property *property, void *new_value, size_t new_value_size);
/* [한국어] set 콜백 타입 — decoded 값을 실제로 적용하는 단계.
 * mngt: ftl_mngt 파이프라인 핸들 — 콜백 본체가 ftl_mngt_next_step(mngt)을 반드시 호출해야 다음 단계 진행.
 * 비동기 작업이 필요한 프로퍼티는 콜백 안에서 작업 시작 후 완료 시 next_step 호출. */

/**
 * @brief Register a FTL property
 *
 * @param dev FTL device
 * @param name the FTL property name
 * @param value Pointer to the value of property
 * @param size The value size of the property
 * @param unit The unit of the property value
 * @param desc The property description for user help
 * @param dump The function to dump the property to the JSON RPC request
 * @param decode The function to decode a new value of the property
 * @param set The function to execute the property setting procedure
 * @param verbose_mode The property is available in verbose mode only
 */
/*
 * [한국어]
 * ftl_property_register - 새 프로퍼티를 dev->properties LIST에 등록
 *
 * @param dev: FTL 디바이스.
 * @param name: 프로퍼티 이름(전역 const 문자열 — 등록 후 라이프타임 유지 필수).
 * @param value: 실제 값을 가리키는 포인터(예: dev 안의 어떤 필드).
 * @param size: 값 크기(바이트).
 * @param unit: 단위 문자열("ms", "GiB" 등) — JSON dump 시 표시.
 * @param desc: 사용자 도움말 설명.
 * @param dump: dump 콜백(필수).
 * @param decode: decode 콜백(NULL이면 read-only).
 * @param set: set 콜백(NULL이면 read-only).
 * @param verbose_mode: true이면 dev->conf.verbose_mode일 때만 노출.
 *
 * 동기/배경: 디바이스 init mngt 중 각 모듈이 자신의 튜너블을 등록한다.
 * 에러 경로: 동일 이름 중복 또는 OOM 시 ftl_abort()로 즉시 종료(복구 의미가 없는 코딩 오류이므로).
 */
void ftl_property_register(struct spdk_ftl_dev *dev,
			   const char *name, void *value, size_t size,
			   const char *unit, const char *desc,
			   ftl_property_dump_fn dump,
			   ftl_property_decode_fn decode,
			   ftl_property_set_fn set,
			   bool verbose_mode);

/**
 * @brief Dump FTL properties to the JSON request
 *
 * @param dev FTL device
 * @param request The JSON request where to store the FTL properties
 */
/*
 * [한국어]
 * ftl_property_dump - 디바이스의 모든 visible 프로퍼티를 JSON RPC 응답으로 dump
 *
 * @param dev: FTL 디바이스.
 * @param request: SPDK JSON-RPC request — 응답 객체 시작/종료를 이 함수가 관리.
 *
 * 동작: spdk_jsonrpc_begin_result로 응답 시작 → "name": dev name, "properties": [ ... ] 배열에
 *   각 visible 프로퍼티를 dump → spdk_jsonrpc_end_result.
 * 실행 컨텍스트: JSON-RPC 핸들러 스레드(SPDK reactor).
 */
void ftl_property_dump(struct spdk_ftl_dev *dev, struct spdk_jsonrpc_request *request);

/**
 * @brief Decode property value and store it in output
 *
 * @param dev FTL device
 * @param name The property name to be decoded
 * @param value The new property value
 * @param value_size The new property value buffer size
 * @param output The output where to store new value
 * @param output_size The decoded value output size
 */
/*
 * [한국어]
 * ftl_property_decode - 이름으로 프로퍼티 찾고 decode 콜백을 호출해 새 값을 binary로 변환
 *
 * @param dev: FTL 디바이스.
 * @param name: 프로퍼티 이름.
 * @param value: 사용자 입력 문자열.
 * @param value_size: 입력 크기.
 * @param[out] output: 디코드된 binary 값 버퍼 (이 함수가 calloc 후 *output에 저장).
 * @param[out] output_size: 위 버퍼 크기.
 * @return: 0 = 성공, -ENOENT = 이름 없음, -EACCES = read-only/inactive, -EINVAL = decode 실패.
 *
 * 에러 경로: 실패 시 *output free하고 NULL로 set 후 음수 반환. 호출자는 free(*output) 책임(성공 후).
 */
int ftl_property_decode(struct spdk_ftl_dev *dev, const char *name, const char *value,
			size_t value_size, void **output, size_t *output_size);

/**
 * @brief The property bool decoder
 */
/*
 * [한국어]
 * ftl_property_decode_bool - "true"/"false" 문자열을 bool로 디코드
 *
 * @param dev/property: 표준 시그니처(미사용).
 * @param value/value_size: "true" 또는 "false" 문자열.
 * @param output: bool* 포인터 — *output에 결과 저장.
 * @param output_size: sizeof(bool)이어야 함.
 * @return: 0 = 성공, -ENOBUFS = 크기 불일치, -EINVAL = 문자열 매칭 실패/비-NUL 종료.
 */
int ftl_property_decode_bool(struct spdk_ftl_dev *dev, struct ftl_property *property,
			     const char *value, size_t value_size, void *output, size_t output_size);

/**
 * @brief Set FTL property
 *
 * @param dev FTL device
 * @param mngt FTL management process handle
 * @param name The property name to be set
 * @param value The new property decoded value
 * @param output The size of the new property decoded value
 */
/*
 * [한국어]
 * ftl_property_set - 디코드된 값을 mngt 통해 안전하게 적용
 *
 * @param dev: FTL 디바이스.
 * @param mngt: ftl_mngt 파이프라인 핸들 — set 콜백이 next_step으로 진행 신호를 보냄.
 * @param name: 프로퍼티 이름.
 * @param value: 디코드된 binary 값(ftl_property_decode 결과).
 * @param value_size: value 크기(바이트).
 * @return: 0 = 성공(콜백 호출 성공), -ENOENT/-EACCES = 이름 없음/권한 없음.
 *
 * 동작: 이름으로 프로퍼티 매칭 → set 콜백 존재/visibility 검증 → property->set(dev, mngt, ...) 호출.
 *   콜백이 실제로 값을 갱신하고 mngt next_step을 호출.
 */
int ftl_property_set(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
		     const char *name, void *value, size_t value_size);

/**
 * @brief Generic setter of the property
 *
 * @note This setter does binary copy and finishes always call the next management step
 */
/*
 * [한국어]
 * ftl_property_set_generic - 단순 memcpy + ftl_mngt_next_step 호출의 표준 setter
 *
 * @param dev/mngt/property: 표준 시그니처.
 * @param new_value: 새 값 binary 데이터.
 * @param new_value_size: 새 값 크기 — property->size와 반드시 일치(아니면 ftl_bug).
 *
 * 동기/배경: 대부분의 프로퍼티는 단순 binary 복사로 충분하므로 이 함수가 기본 setter.
 *   복잡한 부수효과(예: I/O 채널 재구성)가 필요한 프로퍼티만 자체 setter를 제공.
 */
void ftl_property_set_generic(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
			      const struct ftl_property *property,
			      void *new_value, size_t new_value_size);

/**
 * @brief The wrapper function to register mutable boolean property
 *
 * @param dev FTL device
 * @param name The property name
 * @param value The pointer to the boolean value of the property
 * @param unit The property unit
 * @param desc The property description
 * @param verbose_mode The verbose mode flag
 */
/*
 * [한국어]
 * ftl_property_register_bool_rw - read-write bool 프로퍼티를 한 줄로 등록하는 인라인 wrapper
 *
 * @param dev: FTL 디바이스.
 * @param name: 프로퍼티 이름.
 * @param value: bool* — 갱신 대상.
 * @param unit/desc: 단위 + 설명.
 * @param verbose_mode: true면 verbose 모드에서만 노출.
 *
 * 동작: ftl_property_register에 표준 콜백 (dump_bool, decode_bool, set_generic)를 자동 결합해 호출.
 *   이로써 호출자가 매번 같은 콜백 트리오를 적기 전 boilerplate를 제거.
 */
static inline void
ftl_property_register_bool_rw(struct spdk_ftl_dev *dev, const char *name, bool *value,
			      const char *unit, const char *desc, bool verbose_mode)
{
	ftl_property_register(dev, name, value, sizeof(*value), unit, desc, ftl_property_dump_bool,
			      ftl_property_decode_bool, ftl_property_set_generic, verbose_mode);
	/* [한국어] 표준 콜백 트리오로 ftl_property_register 호출:
	 *   - dump: ftl_property_dump_bool (JSON named bool 출력).
	 *   - decode: ftl_property_decode_bool ("true"/"false" 문자열 → bool).
	 *   - set: ftl_property_set_generic (단순 memcpy + next_step).
	 *  size 인자는 sizeof(*value) = sizeof(bool) — 디코드/set에서 검증 사용. */
}

#endif
/* [한국어] 헤더 가드 종료. */
