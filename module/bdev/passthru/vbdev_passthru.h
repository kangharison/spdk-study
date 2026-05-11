/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] passthru vbdev 모듈 공개 헤더 (vbdev_passthru.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK passthru(통과형) 가상 bdev 모듈이 외부(주로 RPC 핸들러)에
 * 노출하는 두 개의 진입점 — passthru 디스크 생성/삭제 함수의 선언만 담는다.
 * passthru 모듈 자체는 base bdev 위에 같은 크기/특성을 가진 vbdev를 얇게 한 겹
 * 얹어 모든 I/O를 그대로 하부로 전달하는 "스켈레톤" 모듈이며, 새로운 vbdev 모듈을
 * 작성할 때 가장 먼저 참고하는 표준 레퍼런스 구현이다.
 * 따라서 이 헤더는 모듈 내부 자료구조(구현부의 vbdev_passthru, pt_io_channel 등)는
 * 노출하지 않고, RPC 계층이 호출할 두 함수만 외부에 공개한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK bdev 스택은 [bdev_io 발행자] → [bdev 코어 (lib/bdev)] → [bdev 모듈
 * (module/bdev/<X>)] → [base bdev/디바이스] 의 4단 구조를 가진다. passthru 모듈은
 * 이 중 "bdev 모듈" 자리에 끼어드는 가상 디바이스(vbdev)로, 실제 데이터는 자기
 * 밑의 base bdev에 의존한다.
 * 호출 체인: 사용자 → JSON-RPC ("bdev_passthru_create"/"bdev_passthru_delete")
 * → vbdev_passthru_rpc.c 의 RPC 핸들러 → 본 헤더가 선언한
 * bdev_passthru_create_disk()/bdev_passthru_delete_disk() → vbdev_passthru.c 구현부
 * → bdev 코어(spdk_bdev_register/unregister, spdk_bdev_open_ext) →
 * base bdev(예: malloc/aio/nvme bdev).
 * 실행 컨텍스트: 두 함수 모두 호스트 유저스페이스의 SPDK app 스레드(보통
 * RPC 스레드 또는 init 스레드)에서 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: <spdk/stdinc.h>(공통 표준 헤더 추상화), <spdk/bdev.h>(bdev 일반 API,
 * spdk_bdev/spdk_bdev_desc 등), <spdk/bdev_module.h>(spdk_bdev_module/
 * spdk_bdev_fn_table/spdk_bdev_unregister_cb 등 모듈 작성에 필요한 내부 타입).
 * 본 헤더에 의존하는 측: vbdev_passthru.c (구현 — 함수 정의 측), vbdev_passthru_rpc.c
 * (RPC 핸들러 — 함수 호출 측). 데이터 흐름은 RPC가 받은 (base_bdev_name, vbdev_name,
 * uuid)을 그대로 본 함수들로 흘려보내고, vbdev_passthru.c가 spdk_bdev_register()를
 * 호출하면 SPDK 글로벌 bdev 리스트에 새 passthru bdev가 등록된다. 삭제 시는 비동기
 * unregister 콜백(spdk_bdev_unregister_cb) 패턴으로 결과를 RPC로 되돌린다.
 *
 * === 주요 함수/구조체 요약 ===
 * - bdev_passthru_create_disk(): base bdev 위에 새 passthru vbdev를 동기 생성.
 *   RPC 'bdev_passthru_create'의 백엔드. UUID 미지정 시 자동 생성된 UUID 사용.
 * - bdev_passthru_delete_disk(): 이름으로 passthru vbdev를 비동기 삭제. unregister가
 *   완료되면 cb_fn(cb_arg, errno)를 호출하여 결과를 알린다.
 * 본 헤더에는 구조체 정의는 없다 — passthru 내부 상태(vbdev_passthru, pt_io_channel)는
 * 구현 .c 파일에 캡슐화되어 외부에 보이지 않는다.
 */

#ifndef SPDK_VBDEV_PASSTHRU_H
/* [한국어] 헤더 가드 시작 — 같은 컴파일 단위에서 본 헤더가 두 번 포함되어
 * 함수 선언이 중복 선언되는 것을 방지. 매크로 이름은 파일 경로의 대문자 변형. */
#define SPDK_VBDEV_PASSTHRU_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 셋 — <stdio.h>, <stdint.h>, <pthread.h> 등
 * 호스트 환경에 따라 정상화된 표준 헤더를 한 번에 끌어온다. 본 헤더는 직접
 * 표준 타입을 쓰지 않더라도, 아래 bdev 헤더들이 표준 타입을 의존하므로 필수. */

#include "spdk/bdev.h"
/* [한국어] bdev 공개 API — spdk_bdev, spdk_bdev_desc, spdk_uuid 등의 타입과
 * spdk_bdev_open_ext/close 같은 호출자가 사용하는 기본 함수 선언을 제공.
 * 본 헤더에서는 'struct spdk_uuid *' 파라미터 타입 때문에 필요. */
#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 작성용 내부 API — spdk_bdev_module 등록 매크로,
 * spdk_bdev_fn_table, spdk_bdev_unregister_cb(unregister 완료 콜백 시그니처)
 * 등을 노출. delete 함수의 콜백 타입이 여기서 온다. */

/**
 * Create new pass through bdev.
 *
 * \param bdev_name Bdev on which pass through vbdev will be created.
 * \param vbdev_name Name of the pass through bdev.
 * \param uuid Optional UUID to assign to the pass through bdev.
 * \return 0 on success, other on failure.
 */
/*
 * [한국어]
 * bdev_passthru_create_disk - base bdev 위에 새 passthru vbdev 한 개를 생성한다.
 *
 * @bdev_name: 위에 얹을 base bdev의 이름. 호출 시점에 base bdev가 SPDK에 등록되어
 *             있어야 한다(없으면 -ENODEV 등 음수 반환). RPC 핸들러가 사용자에게서
 *             받은 문자열을 그대로 넘기는 형태이며, 함수 내부에서 복사되어 들어가므로
 *             호출자가 free 책임을 유지한다.
 * @vbdev_name: 새로 만들 passthru vbdev의 이름. 글로벌 bdev 네임스페이스에서 유일해야
 *              하며, 중복이면 spdk_bdev_register()가 -EEXIST를 돌려준다.
 * @uuid: 선택적 UUID. NULL/0이 아닌 값이 들어오면 spdk_bdev->uuid에 그대로 사용,
 *        비어 있으면 SPDK가 random UUID를 자동 생성한다.
 * @return: 0 — 성공, 음수 errno — 실패(-ENODEV, -ENOMEM, -EEXIST 등).
 *
 * 동기/배경: 새 vbdev를 등록하려면 (1) base bdev 디스크립터 오픈 → (2) claim →
 * (3) spdk_bdev 객체 채우기 → (4) spdk_bdev_register() 순서가 필요하다. RPC가
 * 받은 입력만으로 이 시퀀스를 일관되게 수행하기 위한 모듈 외부의 단일 진입점이다.
 * 동기 함수처럼 보이지만 내부적으로 SPDK examine 단계에서 실제 등록이 완료될 수
 * 있다 — 반환 0은 "등록 요청이 받아들여졌다"는 의미.
 * 실행 컨텍스트: SPDK app 스레드(JSON-RPC 처리 스레드). 잠금은 bdev 코어가 관리.
 *
 * 호출 체인:
 *   rpc_bdev_passthru_create() → [bdev_passthru_create_disk] →
 *     vbdev_passthru_register() → spdk_bdev_module_claim_bdev_desc() →
 *     spdk_bdev_register() → (이후 I/O는 spdk_bdev_fn_table 콜백으로 진입)
 */
int bdev_passthru_create_disk(const char *bdev_name, const char *vbdev_name,
			      const struct spdk_uuid *uuid);
/* [한국어] 위 주석 참고 — 외부에 공개되는 생성 진입점 선언. */

/**
 * Delete passthru bdev.
 *
 * \param bdev_name Name of the pass through bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
/*
 * [한국어]
 * bdev_passthru_delete_disk - 이름으로 passthru vbdev를 비동기로 제거한다.
 *
 * @bdev_name: 삭제할 passthru vbdev 이름(create 시 사용한 vbdev_name).
 *             내부적으로 spdk_bdev_get_by_name()으로 spdk_bdev 핸들을 찾는다.
 *             없으면 cb_fn(-ENODEV)으로 즉시 알림.
 * @cb_fn: 삭제 완료 알림 콜백. 시그니처는 spdk_bdev_unregister_cb 타입
 *         (void (*)(void *cb_arg, int bdeverrno)). 0=성공, 음수=errno.
 * @cb_arg: cb_fn에 그대로 전달되는 사용자 컨텍스트. 보통 RPC request 객체 포인터.
 * @return: 없음 — 결과는 항상 cb_fn으로만 전달된다(비동기 모델).
 *
 * 동기/배경: bdev 삭제는 (a) 사용자(open 디스크립터들) 모두 닫힘 (b) I/O 채널 정리
 * (c) base bdev claim 해제 가 필요해 즉시 반환하기 어렵다. SPDK는 unregister 후
 * 비동기 콜백으로 결과를 통지한다. 본 함수는 내부에서 spdk_bdev_unregister()를
 * 호출하고 즉시 반환한다.
 * 실행 컨텍스트: 호출은 SPDK app 스레드(보통 RPC 스레드)에서. cb_fn 실행 컨텍스트는
 * SPDK가 unregister를 처리한 스레드 — 일반적으로 init 스레드에서 호출된다.
 *
 * 호출 체인:
 *   rpc_bdev_passthru_delete() → [bdev_passthru_delete_disk] →
 *     spdk_bdev_unregister() → (비동기) → vbdev_passthru_destruct() →
 *     cb_fn(cb_arg, bdeverrno) → rpc_bdev_passthru_delete_cb() → JSON-RPC 응답.
 */
void bdev_passthru_delete_disk(const char *bdev_name, spdk_bdev_unregister_cb cb_fn,
			       void *cb_arg);
/* [한국어] 위 주석 참고 — 외부에 공개되는 비동기 삭제 진입점 선언. */

#endif /* SPDK_VBDEV_PASSTHRU_H */
/* [한국어] 헤더 가드 종료. */
