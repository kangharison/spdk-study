/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] error vbdev 모듈 공개 헤더 (vbdev_error.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 error(에러 주입) 가상 bdev 모듈이 외부에 노출하는 타입과 함수를 선언한다.
 * error 모듈은 base bdev 위에 얹혀, 사용자가 지정한 횟수/큐 깊이 임계점에 따라
 * I/O를 인위적으로 실패(failure), 보류(pending), 데이터 손상(corrupt_data),
 * 메모리 부족(NOMEM)으로 만들어 상위 계층의 에러 처리 경로를 검증할 수 있게 한다.
 * 본 헤더는 (1) 에러 종류 식별 enum, (2) 삭제 완료 콜백 typedef,
 * (3) error vbdev 생성/삭제 함수, (4) 에러 주입 옵션 구조체와 주입 함수를 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK bdev 스택의 vbdev 자리. I/O 흐름:
 *   상위 → bdev 코어 → [error vbdev: bdev_io 받음] → 주입 정책 평가 →
 *   실패 시 spdk_bdev_io_complete(io, FAILED)/(SUCCESS+corrupt) 또는 NOMEM 반환,
 *   정상 시 base bdev로 재발행.
 * 즉, 결함 주입은 "발행" 시점에 인터셉트되어 base로 가지 않을 수도 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: <spdk/stdinc.h>, <spdk/bdev.h>(spdk_bdev/spdk_bdev_io_type), <spdk/uuid.h>(spdk_uuid).
 * 의존하는 측: vbdev_error.c, vbdev_error_rpc.c.
 * RPC 핸들러는 (1) 'bdev_error_create'에서 본 헤더의 vbdev_error_create()를 부르고,
 * (2) 'bdev_error_delete'에서 vbdev_error_delete()를 부르며,
 * (3) 'bdev_error_inject_error'에서 vbdev_error_inject_opts를 채워
 *     vbdev_error_inject_error()로 결함 정책을 동적으로 주입한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - enum vbdev_error_type: 어떤 종류의 에러를 주입할지 식별.
 * - spdk_delete_error_complete: error vbdev 삭제 완료 콜백 typedef.
 * - vbdev_error_create(): error vbdev 생성.
 * - vbdev_error_delete(): error vbdev 비동기 삭제.
 * - struct vbdev_error_inject_opts: 결함 주입 정책(io_type, error_type, error_num,
 *   error_qd, corrupt_offset, corrupt_value)을 한 번에 묶은 옵션 구조체.
 * - vbdev_error_inject_error(): 위 옵션을 적용해 런타임 결함 주입을 시작.
 */

#ifndef SPDK_VBDEV_ERROR_H
/* [한국어] 헤더 가드 시작. */
#define SPDK_VBDEV_ERROR_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 헤더 셋 — uint32/64_t, uint8_t 등의 타입 정상화. */
#include "spdk/bdev.h"
/* [한국어] bdev 공개 API — spdk_bdev/spdk_bdev_io_type 정의 (io_type 필드의 의미가
 * SPDK_BDEV_IO_TYPE_READ 등의 값으로 해석되기 때문). */
#include "spdk/uuid.h"
/* [한국어] spdk_uuid 타입 — vbdev_error_create의 uuid 파라미터에 필요. */

enum vbdev_error_type {
	/* [한국어] 결함 주입 카테고리 — 어떤 종류의 에러를 만들어낼지 결정.
	 * RPC가 사용자에게서 받은 문자열 "failure"/"pending"/"corrupt_data"/"nomem"을
	 * 이 enum으로 매핑한 뒤 vbdev_error_inject_opts.error_type에 저장한다. */
	VBDEV_IO_NO_ERROR = 0,
	/* [한국어] 에러 없음 — 카운터가 0이 되거나 기존 정책을 'clear' 할 때의 sentinel.
	 * 명시적으로 0 값을 부여해 외부와 비트 마스크 호환을 유지. */
	VBDEV_IO_FAILURE,
	/* [한국어] I/O를 즉시 실패로 보고 — spdk_bdev_io_complete()에 SPDK_BDEV_IO_STATUS_FAILED
	 * 를 넘긴다. 상위가 errno를 어떻게 처리하는지 검증할 때 사용. */
	VBDEV_IO_PENDING,
	/* [한국어] I/O를 영원히 완료시키지 않고 보류 — 타임아웃/응답 없는 디바이스 시뮬레이션. */
	VBDEV_IO_CORRUPT_DATA,
	/* [한국어] 데이터 손상 시뮬레이션 — write 시 corrupt_offset 위치에 corrupt_value를
	 * 덮어쓰거나, read 시 반환 버퍼를 손상. 상위 검증(체크섬/ECC) 경로 테스트에 사용. */
	VBDEV_IO_NOMEM,
	/* [한국어] 메모리 부족 시뮬레이션 — bdev 코어에 SPDK_BDEV_IO_STATUS_NOMEM을 보고하여
	 * 재시도 큐 동작을 검증. */
};

typedef void (*spdk_delete_error_complete)(void *cb_arg, int bdeverrno);
/* [한국어] error vbdev 삭제 완료 콜백 시그니처. spdk_bdev_unregister_cb와 동일 형태이지만
 * error 모듈 자체 typedef로 별도 노출하여, 호출자가 간단히 이 타입의 함수 포인터를
 * 정의할 수 있게 한다. cb_arg는 호출자가 등록한 컨텍스트, bdeverrno는 0=성공, 음수=errno. */

/**
 * Create a vbdev on the base bdev to inject error into it.
 *
 * \param base_bdev_name Name of the base bdev.
 * \param uuid Optional UUID to assign to the bdev.
 * \return 0 on success or negative on failure.
 */
/*
 * [한국어]
 * vbdev_error_create - base bdev 위에 error vbdev 한 개를 생성.
 *
 * @base_bdev_name: 결함을 주입할 base bdev 이름. 등록돼 있어야 한다.
 * @uuid: 선택적 UUID. NULL/0이면 자동 생성.
 * @return: 0 — 성공, 음수 errno — 실패.
 *
 * 동기/배경: 결함 주입은 vbdev로 모델링하는 것이 자연스럽다 — base bdev는 그대로
 * 두고, 위에 한 겹 얹어 결함 정책을 적용한다. 실제 결함 정책은 추후
 * vbdev_error_inject_error()로 동적으로 등록한다.
 * 실행 컨텍스트: SPDK app 스레드(RPC).
 *
 * 호출 체인:
 *   rpc_bdev_error_create() → [vbdev_error_create] → spdk_bdev_register().
 */
int vbdev_error_create(const char *base_bdev_name, const struct spdk_uuid *uuid);
/* [한국어] 위 주석 참고 — error vbdev 생성 진입점. */

/**
 * Delete vbdev used to inject errors.
 *
 * \param error_vbdev_name Name of the error vbdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Arguments to pass to cb_fn.
 */
/*
 * [한국어]
 * vbdev_error_delete - error vbdev를 비동기 삭제.
 *
 * @error_vbdev_name: 제거할 error vbdev 이름.
 * @cb_fn: 삭제 완료 콜백 (spdk_delete_error_complete 시그니처).
 * @cb_arg: cb_fn에 전달될 사용자 컨텍스트(보통 RPC request 포인터).
 * @return: 없음 — 결과는 cb_fn으로만.
 *
 * 호출 체인:
 *   rpc_bdev_error_delete() → [vbdev_error_delete] → spdk_bdev_unregister() →
 *     vbdev_error_destruct() → cb_fn(cb_arg, errno).
 */
void vbdev_error_delete(const char *error_vbdev_name, spdk_delete_error_complete cb_fn,
			void *cb_arg);
/* [한국어] 위 주석 참고 — 비동기 삭제 진입점. */

struct vbdev_error_inject_opts {
	/* [한국어] 결함 주입 정책을 한 번에 묶은 옵션 구조체. RPC가 JSON에서 디코드한
	 * 7개의 필드를 그대로 vbdev_error_inject_error()로 전달한다. 각 필드는 독립적으로
	 * 의미를 가지며, 일부는 옵셔널이다. */
	uint32_t io_type;
	/* [한국어] 어떤 I/O 종류에 결함을 적용할지 — SPDK_BDEV_IO_TYPE_READ/WRITE/
	 * FLUSH/UNMAP 값을 가지며, 0xffffffff은 'all'(모든 종류), 0은 'clear'(주입 해제).
	 * 설정자: RPC rpc_error_bdev_decode_io_type(). 읽는 자: 모듈 I/O 핸들러. */
	uint32_t error_type;
	/* [한국어] 어떤 종류의 에러를 만들지 — vbdev_error_type enum 값.
	 * 설정자: RPC rpc_error_bdev_decode_error_type(). */
	uint32_t error_num;
	/* [한국어] 결함을 주입할 횟수. 1 이상이면 카운트다운, 0이면 활성 해제.
	 * 옵셔널 — 기본 1. */
	uint64_t error_qd;
	/* [한국어] 결함을 트리거할 큐 깊이 임계 — 현재 outstanding I/O 수가 이 값에
	 * 도달하면 다음 I/O를 결함 처리. 0이면 큐 깊이 조건을 사용하지 않고 단순
	 * 카운트다운 모드. 옵셔널. */
	uint64_t corrupt_offset;
	/* [한국어] CORRUPT_DATA 시 데이터 버퍼의 어느 바이트를 손상시킬지 오프셋(바이트).
	 * VBDEV_IO_CORRUPT_DATA에서만 의미가 있음. 옵셔널. */
	uint8_t corrupt_value;
	/* [한국어] CORRUPT_DATA 시 corrupt_offset 위치에 XOR 또는 덮어쓸 1바이트 값.
	 * 옵셔널 — 기본 0. */
};

/**
 * Inject error to the base bdev. Users can specify which IO type error is injected,
 * what type of error is injected, and how many errors are injected.
 *
 * \param name Name of the base bdev into which error is injected.
 * \param opts Options for error injection.
 */
/*
 * [한국어]
 * vbdev_error_inject_error - 런타임에 결함 주입 정책을 활성/갱신.
 *
 * @name: 정책을 적용할 error vbdev(또는 base name) — 모듈 내부에서 매핑.
 * @opts: 결함 정책을 담은 vbdev_error_inject_opts 구조체 포인터.
 * @return: 0 — 성공, 음수 errno — 실패.
 *
 * 동기/배경: 정책을 한 번 등록하면 모듈 내부 카운터(error_num/error_qd)에 따라
 * 매 I/O마다 결함 여부가 결정되고, 횟수 소진 후 자동으로 비활성화된다.
 * 실행 컨텍스트: SPDK app 스레드(RPC). 정책 갱신과 I/O 평가가 같은 스레드에서
 * 일어나도록 모듈 내부에서 보장.
 *
 * 호출 체인:
 *   rpc_bdev_error_inject_error() → [vbdev_error_inject_error] →
 *     모듈 내부 정책 테이블 갱신 → 다음 I/O부터 적용.
 */
int vbdev_error_inject_error(char *name, const struct vbdev_error_inject_opts *opts);
/* [한국어] 위 주석 참고 — 결함 주입 정책 갱신 진입점. */

#endif /* SPDK_VBDEV_ERROR_H */
/* [한국어] 헤더 가드 종료. */
