/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] delay vbdev 모듈 공개 헤더 (vbdev_delay.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 delay(지연 주입) 가상 bdev 모듈이 외부(JSON-RPC 핸들러)에 노출하는
 * 함수와 타입을 선언한다. delay 모듈은 base bdev 위에 얹혀 모든 I/O를 그대로
 * 통과시키되, 완료 시점에 사용자가 지정한 평균 / p99 지연(마이크로초)을 인위적으로
 * 추가한다 — QoS·SLA 테스트, 지연 민감도 분석에 쓰는 도구다.
 * 본 헤더는 (1) 어떤 종류의 지연을 갱신할지 식별하는 enum delay_io_type,
 * (2) delay vbdev 생성/삭제 진입점, (3) 런타임에 지연 값을 갱신하는 진입점만 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK bdev 스택의 vbdev 자리. I/O 흐름은:
 *   상위 모듈/앱 → bdev 코어 → [delay vbdev: bdev_io 받음] →
 *   base bdev에 즉시 재발행 → base 완료 시 [delay vbdev: 완료를 지연 큐에 push] →
 *   poller가 지연 만료 후 상위에 spdk_bdev_io_complete().
 * 즉, 지연은 "발행" 시가 아니라 "완료 보고" 시점에 인위적으로 늘려 추가된다.
 * 실행 컨텍스트: 본 헤더의 함수들은 모두 SPDK app 스레드(JSON-RPC 처리 스레드)에서
 * 호출되며, I/O 처리는 각 채널의 reactor 스레드에서 polled-mode로 이뤄진다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: <spdk/stdinc.h>(표준 헤더 셋), <spdk/bdev.h>(spdk_bdev/spdk_uuid),
 *       <spdk/bdev_module.h>(spdk_bdev_unregister_cb 콜백 타입).
 * 본 헤더에 의존하는 측: vbdev_delay.c (구현), vbdev_delay_rpc.c (RPC 핸들러).
 * 데이터 흐름: RPC가 받은 (base_bdev_name, vbdev_name, uuid, 4개 지연값) →
 * create_delay_disk() → vbdev_delay 모듈 내부 구조체에 저장. 런타임에는
 * vbdev_delay_update_latency_value()로 지연 파라미터만 핫스왑 가능.
 * 공유 구조체: 외부에는 enum delay_io_type만 노출 — 실제 vbdev_delay/delay_io_channel
 * 구조체는 .c 파일에 비공개.
 *
 * === 주요 함수/구조체 요약 ===
 * - enum delay_io_type: 어떤 카테고리의 지연을 갱신할지 식별(평균/p99 × 읽기/쓰기).
 * - create_delay_disk(): 새 delay vbdev를 base 위에 생성. 4개의 지연값을 모두 받음.
 * - delete_delay_disk(): 비동기 삭제. unregister 완료 시 cb_fn으로 결과 통지.
 * - vbdev_delay_update_latency_value(): 런타임에 지연 1종 파라미터만 변경.
 */

#ifndef SPDK_VBDEV_DELAY_H
/* [한국어] 헤더 가드 시작. */
#define SPDK_VBDEV_DELAY_H

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 인클루드 셋 — uint64_t 등의 타입을 정상화하는 데 필요. */

#include "spdk/bdev.h"
/* [한국어] bdev 공개 API — spdk_bdev/spdk_uuid 타입을 사용. */
#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 작성용 내부 API — spdk_bdev_unregister_cb 시그니처 제공. */

enum delay_io_type {
	/* [한국어] delay 카테고리 — 어떤 종류의 지연 파라미터를 가리키는지 식별하는
	 * enum. RPC에서 사용자가 지정하는 latency_type 문자열을 이 enum으로 매핑하고,
	 * 모듈 내부 4개의 지연 값(평균 읽기, p99 읽기, 평균 쓰기, p99 쓰기) 중 하나를
	 * 선택한다. 모든 I/O는 평균 분포 + p99 외부값 두 가지 중 하나로 분류되어
	 * 해당 지연이 적용된다. */
	DELAY_AVG_READ,
	/* [한국어] 읽기 I/O의 평균(typical) 지연 — 대부분의 read에 적용. */
	DELAY_P99_READ,
	/* [한국어] 읽기 I/O 중 99 percentile 외부에 해당하는 슬로우 지연 — 1% 비율로 적용. */
	DELAY_AVG_WRITE,
	/* [한국어] 쓰기 I/O의 평균 지연. */
	DELAY_P99_WRITE,
	/* [한국어] 쓰기 I/O의 p99 슬로우 지연. */
	DELAY_NONE
	/* [한국어] 미지정/무효 카테고리 — 매핑 실패 시의 sentinel 값. */
};

/**
 * Create new delay bdev.
 *
 * \param bdev_name Bdev on which delay vbdev will be created.
 * \param vbdev_name Name of the delay bdev.
 * \param uuid UUID of the delay bdev.
 * \param avg_read_latency Desired typical read latency.
 * \param p99_read_latency Desired p99 read latency
 * \param avg_write_latency Desired typical write latency.
 * \param p99_write_latency Desired p99 write latency
 * \return 0 on success, other on failure.
 */
/*
 * [한국어]
 * create_delay_disk - delay vbdev 한 개를 base 위에 생성하고 초기 지연값 4종을 설정.
 *
 * @bdev_name: base bdev 이름. 등록돼 있어야 한다.
 * @vbdev_name: 새 delay vbdev 이름(글로벌 유일).
 * @uuid: 선택적 UUID 포인터. NULL/0이면 자동 생성.
 * @avg_read_latency: 평균 읽기 지연(us). 0이면 사실상 지연 없음.
 * @p99_read_latency: p99 읽기 지연(us). avg보다 커야 의미가 있다.
 * @avg_write_latency: 평균 쓰기 지연(us).
 * @p99_write_latency: p99 쓰기 지연(us).
 * @return: 0 — 성공, 음수 errno — 실패(-ENODEV, -ENOMEM, -EEXIST, -EINVAL).
 *
 * 동기/배경: I/O 모니터링 도구를 검증하거나 SLA를 흉내내려면 인위적 지연을 주입해
 * 결과를 관측해야 한다. delay vbdev는 모든 I/O를 통과시키되 완료를 지연시켜
 * 이 효과를 만든다. 4개 지연값을 모두 한 번에 받아 모듈 내부 구조체(vbdev_delay)에
 * 저장한다. 이후 런타임 갱신은 vbdev_delay_update_latency_value()로 가능.
 * 실행 컨텍스트: SPDK app 스레드(JSON-RPC 또는 init).
 *
 * 호출 체인:
 *   rpc_bdev_delay_create() → [create_delay_disk] → vbdev_delay_register() →
 *     spdk_bdev_register().
 */
int create_delay_disk(const char *bdev_name, const char *vbdev_name, struct spdk_uuid *uuid,
		      uint64_t avg_read_latency,
		      uint64_t p99_read_latency, uint64_t avg_write_latency, uint64_t p99_write_latency);
/* [한국어] 위 주석 참고 — delay vbdev 생성 진입점. */

/**
 * Delete delay bdev.
 *
 * \param vbdev_name Name of the delay bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
/*
 * [한국어]
 * delete_delay_disk - 이름으로 delay vbdev를 비동기 삭제한다.
 *
 * @vbdev_name: 삭제 대상 vbdev 이름.
 * @cb_fn: unregister 완료 콜백 (spdk_bdev_unregister_cb 시그니처).
 * @cb_arg: cb_fn에 그대로 전달되는 사용자 컨텍스트.
 * @return: 없음 — 결과는 cb_fn으로만.
 *
 * 동기/배경: passthru와 동일한 비동기 삭제 모델 — bdev 사용자 모두 닫힘 + I/O 채널
 * 정리 + base claim 해제가 끝나야 destruct 콜백이 cb_fn을 호출한다.
 * 실행 컨텍스트: 호출은 RPC 스레드, cb_fn은 init 스레드.
 *
 * 호출 체인:
 *   rpc_bdev_delay_delete() → [delete_delay_disk] → spdk_bdev_unregister() →
 *     vbdev_delay_destruct() → cb_fn(cb_arg, errno).
 */
void delete_delay_disk(const char *vbdev_name, spdk_bdev_unregister_cb cb_fn,
		       void *cb_arg);
/* [한국어] 위 주석 참고 — 비동기 삭제 진입점. */

/**
 * Update one of the latency values for a given delay bdev.
 *
 * \param delay_name The name of the delay bdev
 * \param latency_us The new latency value, in microseconds
 * \param type a valid value from the delay_io_type enum
 * \return 0 on success, -ENODEV if the bdev cannot be found, and -EINVAL if the bdev is not a delay device.
 */
/*
 * [한국어]
 * vbdev_delay_update_latency_value - 런타임에 지연 4종 중 1종을 핫스왑.
 *
 * @delay_name: 갱신할 delay vbdev 이름.
 * @latency_us: 새 지연 값(us). 0 이상.
 * @type: 갱신할 카테고리 (DELAY_AVG_READ/P99_READ/AVG_WRITE/P99_WRITE 중 하나).
 *        DELAY_NONE을 넘기면 -EINVAL.
 * @return: 0 — 성공, -ENODEV — 이름으로 bdev를 못 찾음, -EINVAL — bdev가 delay
 *          타입이 아니거나 type이 유효하지 않음.
 *
 * 동기/배경: 테스트 시나리오 도중 I/O를 멈추지 않고 지연값만 바꾸고 싶을 때 사용.
 * 갱신 자체는 단순한 변수 대입이지만, I/O 핸들러와 동일 스레드에서 처리되어야
 * 안전하므로 내부에서 spdk_thread_send_msg 등으로 동기화하는 패턴을 쓸 수 있다.
 * 실행 컨텍스트: SPDK app 스레드(RPC). 모듈 내부에서 적절한 스레드로 라우팅.
 *
 * 호출 체인:
 *   rpc_bdev_delay_update_latency() → [vbdev_delay_update_latency_value] →
 *     모듈 내부 vbdev_delay->latency_*_us 필드 갱신.
 */
int vbdev_delay_update_latency_value(char *delay_name, uint64_t latency_us,
				     enum delay_io_type type);
/* [한국어] 위 주석 참고 — 런타임 지연 갱신 진입점. */

#endif /* SPDK_VBDEV_DELAY_H */
/* [한국어] 헤더 가드 종료. */
