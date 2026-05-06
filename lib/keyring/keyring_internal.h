/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

/*
 * [한국어 설명] SPDK keyring 라이브러리 내부 전용 헤더 (keyring_internal.h)
 *
 * === 파일의 역할 ===
 * lib/keyring/ 디렉토리 내부 .c 파일들끼리만 공유하는 사적 인터페이스를 모은
 * 헤더이다. 외부(다른 라이브러리/모듈) 노출용 공개 API는 include/spdk/keyring.h
 * 와 include/spdk/keyring_module.h에 정의되어 있고, 그 두 헤더에 노출하기에는
 * 부적절한(예: dump 형식이 keyring 자체의 RPC 응답 스키마에 강하게 결합된)
 * 함수만 여기서 선언한다. 현재는 keyring_dump_key_info() 한 함수만 있다 —
 * keyring.c가 정의하고 keyring_rpc.c가 RPC 응답을 채울 때 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK keyring 서브시스템(NVMe-oF TLS PSK / NVMe in-band auth(DH-CHAP) /
 * NVMe AES-XTS DEK 등 보안 키를 통합 보관하는 레이어)의 라이브러리 내부 헤더.
 * 호출 체인:
 *   keyring_get_keys RPC → rpc_keyring_for_each_key_cb (keyring_rpc.c)
 *     → keyring_dump_key_info (keyring.c — 본 헤더가 노출하는 함수)
 *       → module->dump_info(key, w) (각 keyring 모듈의 dump 콜백)
 *
 * === 타 모듈과의 연결 ===
 * 의존(헤더): spdk/json.h(spdk_json_write_ctx — RPC 응답을 JSON 스트림으로
 *             직렬화하는 빌더), spdk/keyring.h(struct spdk_key opaque 타입과
 *             keyring 공개 API).
 * 사용처: keyring.c(정의), keyring_rpc.c(호출). 외부 라이브러리는 본 헤더를
 *         include하지 않는다 — Makefile.lib에서 install 대상에 포함되지 않음.
 *
 * === 주요 함수/구조체 요약 ===
 *   - keyring_dump_key_info(key, w):
 *       단일 spdk_key의 메타데이터(name/module/removed/probed/refcnt)와 모듈별
 *       추가 정보를 JSON 객체 필드로 기록한다. 호출자는 객체의 begin/end를
 *       감싸 줘야 하고, 본 함수는 그 안의 named-field만 채운다.
 */

#ifndef SPDK_KEYRING_INTERNAL_H        /* [한국어] include 중복 방지 가드 시작 */
#define SPDK_KEYRING_INTERNAL_H

#include "spdk/json.h"     /* [한국어] spdk_json_write_ctx 정의 — JSON 직렬화 빌더 */
#include "spdk/keyring.h"  /* [한국어] struct spdk_key 등 keyring 공개 자료형 */

/*
 * [한국어]
 * keyring_dump_key_info - 단일 키의 모든 메타데이터를 JSON 필드로 기록
 *
 * @key: 출력할 대상 키. NULL 불가. removed 상태(=백엔드에서 제거되었지만 ref가
 *       남아 g_keyring.removed_keys에 남아 있는 상태)도 허용한다 — 이 경우
 *       module->dump_info는 호출되지 않고 기본 필드만 기록된다.
 * @w:   호출자가 이미 spdk_json_write_object_begin()으로 객체를 연 상태로
 *       전달해야 하는 JSON writer. 본 함수는 객체 내부에 named field들을
 *       추가만 한다 — begin/end는 호출자 책임.
 *
 * 왜 필요한가: keyring_get_keys RPC가 키 목록을 클라이언트(예: spdk_rpc.py)에게
 *   반환할 때 각 키의 식별/상태/모듈 고유 정보를 일관된 스키마로 노출해야 한다.
 *   그 스키마 정의를 keyring 라이브러리 내부에 캡슐화하기 위해 RPC 핸들러
 *   파일이 아닌 keyring 라이브러리 본체(keyring.c)에 정의를 둔다. 그런데 외부
 *   사용자에게는 노출할 이유가 없는 dump 헬퍼이므로 본 internal header에
 *   선언만 둔다.
 *
 * 호출 체인:
 *   rpc_keyring_for_each_key_cb (keyring_rpc.c) → [본 함수] → module->dump_info
 *
 * 동시성: g_keyring.mutex를 잡은 컨텍스트에서 호출됨(spdk_keyring_for_each_key
 *   가 락을 잡은 상태로 fn(ctx, key)를 부르고, fn이 본 함수를 호출).
 */
void keyring_dump_key_info(struct spdk_key *key, struct spdk_json_write_ctx *w);

#endif /* SPDK_KEYRING_INTERNAL_H */    /* [한국어] include 가드 종료 */
