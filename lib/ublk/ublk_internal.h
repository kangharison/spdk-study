/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */
/** \file
 * Userspace block device layer
 */

/*
 * [한국어 설명] SPDK ublk 내부 헤더 (ublk_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK의 ublk(=Userspace Block Device, Linux 5.16+ 커널의 io_uring 기반
 * 사용자공간 블록 디바이스 인터페이스) 통합 모듈에서, lib/ublk/*.c 파일들
 * 사이에서만 공유되는 내부 API와 매크로를 정의한다. 외부 공개 API
 * (`include/spdk/ublk.h`)는 사용자에게 `spdk_ublk_init`/`spdk_ublk_fini` 같은
 * 라이프사이클 함수만 노출하지만, 본 헤더는 RPC 핸들러(ublk_rpc.c)와 ublk
 * 코어 구현(ublk.c) 사이에서 디스크 시작/중지/조회/리커버리 진입점, 그리고
 * 이전 커널 헤더에서 정의되지 않았을 수도 있는 ublk 명령/플래그 호환 매크로를 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK ublk 스택은 다음과 같은 흐름을 가진다:
 *   [Linux 호스트 사용자(애플리케이션)]
 *      ↓ 표준 read/write/ioctl
 *   [Linux 커널 ublk 드라이버 (/dev/ublkbN, /dev/ublkcN)]
 *      ↓ io_uring submission queue (UBLK_IO_FETCH_REQ / COMMIT_AND_FETCH 등)
 *   [SPDK ublk target 프로세스(=본 모듈)]
 *      ↓ ublk_io 변환 → spdk_bdev_io
 *   [SPDK bdev layer (lib/bdev)]
 *      ↓
 *   [bdev 모듈 (nvme/aio/malloc/...)]
 *      ↓
 *   [실제 스토리지 디바이스]
 * 본 헤더는 이 스택의 "SPDK ublk target" 부분 내부에서만 가시화되며, 함수
 * 정의는 없고 선언만 보유한다. 사용자가 이 헤더를 인클루드해선 안 된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `<linux/ublk_cmd.h>`(커널 헤더 — UBLK_IO_*/UBLK_F_* 정의),
 *   `spdk/ublk.h`(공개 API에서 정의되는 spdk_ublk_fini_cb 콜백 타입).
 * - 의존자: `lib/ublk/ublk.c`(코어 구현체 — 본 헤더 함수들의 정의),
 *   `lib/ublk/ublk_rpc.c`(JSON-RPC 핸들러 — 본 헤더 함수들을 호출).
 * - 데이터 흐름: RPC 클라이언트 → ublk_rpc.c가 JSON 파싱 → 본 헤더의 진입점
 *   호출(예: `ublk_start_disk`) → ublk.c가 커널 io_uring으로 ublk 컨트롤 명령 송신.
 * - 공유 자료구조: `struct spdk_ublk_dev`(ublk.c에서 정의되는 불투명 타입).
 *   외부에 핸들로만 전달되며, getter들(`ublk_dev_get_id` 등)을 통해 필드 노출.
 *
 * === 타 모듈과의 연결: 신규/구버전 커널 헤더 호환 ===
 * 본 헤더는 빌드 환경의 커널 헤더가 ublk의 일부 신규 매크로를 정의하지 않는
 * 경우를 위해 fallback 정의를 제공한다(UBLK_F_USER_COPY, UBLK_U_CMD_GET_FEATURES 등).
 * 이는 새로운 SPDK 코드가 오래된 distro의 커널 헤더에서도 컴파일되도록 한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - ublk_create_target / ublk_destroy_target : ublk target 라이프사이클 (전체 모듈 시작/종료).
 * - ublk_start_disk / ublk_stop_disk / ublk_start_disk_recovery : 한 ublk 디스크 단위 lifecycle.
 * - ublk_dev_find_by_id, ublk_dev_first/next, ublk_dev_get_*: ublk 디바이스 enumeration/조회.
 * - typedef ublk_ctrl_cb : 비동기 컨트롤 명령 완료 콜백 (RPC 응답 트리거).
 * - 매크로 UBLK_DEV_QUEUE_DEPTH(128) / UBLK_DEV_NUM_QUEUE(1) : 기본 큐 설정.
 * - 매크로 UBLK_*_BITS / *_OFF : userspace ↔ ublk 커널 간의 tag/qid 인코딩 비트 위치.
 */

#ifndef SPDK_UBLK_INTERNAL_H
#define SPDK_UBLK_INTERNAL_H

/* [한국어] Linux ublk 커널 ABI 헤더. UBLK_IO_FETCH_REQ, UBLK_F_*, ublksrv_io_desc 등
 *  ublk 컨트롤/IO 명령의 정의를 가진다. ublk는 커널 5.16부터 도입되었으므로
 *  빌드 호스트의 커널 헤더 패키지에 포함되어 있어야 한다. */
#include <linux/ublk_cmd.h>

/* [한국어] SPDK 공개 ublk API — `spdk_ublk_fini_cb` 콜백 타입과 `spdk_ublk_init/fini`
 *  선언을 포함. 본 헤더의 ublk_destroy_target이 이 콜백 타입을 인자로 받는다. */
#include "spdk/ublk.h"

/* [한국어] ----- 커널 헤더 호환성 fallback 매크로 -----
 *  아래 #ifndef 가드 블록들은 빌드 호스트의 <linux/ublk_cmd.h>가 더 오래된
 *  버전이라 신규 매크로가 빠져 있을 때를 대비한 정의. 신규 커널과 동일한 값을
 *  미리 박아둠으로써 단일 SPDK 소스가 다양한 커널 버전을 지원하게 한다. */

#ifndef UBLK_F_CMD_IOCTL_ENCODE
/* [한국어] UBLK_F_CMD_IOCTL_ENCODE — ublk 컨트롤 명령을 ioctl 인코딩(_IOR/_IOW 형식)으로
 *  보낼 때 set하는 feature flag (비트 6). 신규 ublk 인터페이스에서 호환성을 위해 도입됨. */
#define UBLK_F_CMD_IOCTL_ENCODE	(1UL << 6)
#endif

#ifndef UBLK_F_USER_COPY
/* [한국어] UBLK_F_USER_COPY — io 데이터를 ublk가 아닌 사용자(=SPDK target)가 직접
 *  카피하는 모드 활성화 플래그(비트 7). 활성화 시 SPDK가 io_uring SQE에 데이터
 *  버퍼 주소를 포함시켜 zero-copy/effective 처리. */
#define UBLK_F_USER_COPY	(1UL << 7)
#endif

#ifndef UBLK_U_CMD_GET_FEATURES
/* [한국어] UBLK_U_CMD_GET_FEATURES — ublk 컨트롤 디바이스에 보내 지원 feature 비트맵을
 *  조회하는 ioctl 명령. _IOR('u', 0x13, ...): 'u' 매직, 0x13 명령 번호, 응답 타입은 ublksrv_ctrl_cmd.
 *  Linux _IO* 매크로는 ioctl 번호 인코딩 표준(<linux/ioctl.h>). */
#define UBLK_U_CMD_GET_FEATURES	_IOR('u', 0x13, struct ublksrv_ctrl_cmd)
#endif

#ifndef UBLKSRV_IO_BUF_OFFSET
/* [한국어] UBLKSRV_IO_BUF_OFFSET — ublk 캐릭터 디바이스(/dev/ublkcN)의 mmap 오프셋 시작점.
 *  IO 버퍼 영역은 이 오프셋 + (qid<<UBLK_QID_OFF | tag<<UBLK_TAG_OFF) 위치에 매핑된다.
 *  값 0x80000000은 ublk 커널 ABI에 정의된 고정 베이스. */
#define UBLKSRV_IO_BUF_OFFSET	0x80000000
#endif

#ifndef UBLK_IO_BUF_BITS
/* [한국어] UBLK_IO_BUF_BITS — IO 버퍼 영역 크기를 표현하는 비트 폭(=25비트, 32MiB).
 *  한 IO 버퍼당 최대 32MiB까지 지원함을 의미. mmap 오프셋 인코딩에 사용. */
#define UBLK_IO_BUF_BITS	25
#endif

#ifndef UBLK_TAG_OFF
/* [한국어] UBLK_TAG_OFF — mmap 오프셋 내에서 tag(=큐 내 IO 슬롯 번호)가 시작되는 비트 위치.
 *  IO 버퍼 비트 위에 위치하므로 25부터 시작. */
#define UBLK_TAG_OFF		UBLK_IO_BUF_BITS
#endif

#ifndef UBLK_TAG_BITS
/* [한국어] UBLK_TAG_BITS — tag의 비트 폭(=16비트). 한 큐당 최대 65536개 tag 가능. */
#define UBLK_TAG_BITS		16
#endif

#ifndef UBLK_QID_OFF
/* [한국어] UBLK_QID_OFF — mmap 오프셋 내에서 queue_id(=큐 인덱스)가 시작되는 비트 위치.
 *  TAG_OFF + TAG_BITS = 25 + 16 = 41. 즉 비트 41부터 qid 영역. */
#define UBLK_QID_OFF		(UBLK_TAG_OFF + UBLK_TAG_BITS)
#endif


/* [한국어] UBLK_DEV_QUEUE_DEPTH — RPC가 queue_depth를 명시하지 않을 때 디폴트 값(128).
 *  한 ublk 큐가 동시에 처리할 수 있는 최대 in-flight IO 수. */
#define UBLK_DEV_QUEUE_DEPTH	128
/* [한국어] UBLK_DEV_NUM_QUEUE — RPC가 num_queues를 명시하지 않을 때 디폴트 값(1).
 *  한 ublk 디바이스의 io_uring 기반 큐 개수(다중 큐 = SMP 분산). */
#define UBLK_DEV_NUM_QUEUE	1

#ifdef __cplusplus
/* [한국어] C++에서 본 헤더를 인클루드해도 C 링크 보장. */
extern "C" {
#endif

/*
 * [한국어] typedef ublk_ctrl_cb — ublk 컨트롤 명령(start/stop) 비동기 완료 콜백 타입.
 *  RPC 핸들러가 이 콜백을 통해 비동기 결과를 클라이언트에 응답하기 위해 사용.
 *
 * @cb_arg: 콜백 컨텍스트(보통 RPC 요청 객체).
 * @result: 0 성공, 음수=errno. RPC 응답으로 그대로 변환됨.
 */
typedef void (*ublk_ctrl_cb)(void *cb_arg, int result);

/*
 * [한국어]
 * ublk_create_target - SPDK 프로세스에서 ublk target을 초기화 (한 번만 호출).
 *
 * @cpumask_str: ublk IO를 처리할 SPDK 스레드를 배치할 CPU 마스크 문자열 (예: "0xF").
 *   NULL 또는 빈 문자열이면 디폴트 마스크 사용.
 * @disable_user_copy: true=UBLK_F_USER_COPY 비활성화(전통 카피 경로),
 *   false=user-copy 활성화 시도(zero-copy 효율).
 * @return: 0 성공 / 음수 errno (이미 초기화됨, 자원 부족, 커널 미지원 등).
 *
 * 사용 패턴: SPDK app 시작 직후 RPC `ublk_create_target`이 호출.
 * 호출 체인: rpc_ublk_create_target → [ublk_create_target] (in lib/ublk/ublk.c)
 */
int ublk_create_target(const char *cpumask_str, bool disable_user_copy);

/*
 * [한국어]
 * ublk_destroy_target - ublk target을 종료하고 모든 ublk 디스크를 정리.
 *
 * @cb_fn: 비동기 종료 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 비동기 진행 시작 / 음수 errno.
 */
int ublk_destroy_target(spdk_ublk_fini_cb cb_fn, void *cb_arg);

/*
 * [한국어]
 * ublk_start_disk - bdev를 ublk 디스크로 노출 (/dev/ublkbN 생성).
 *
 * @bdev_name: 노출할 SPDK bdev 이름.
 * @ublk_id: 부여할 ublk ID(=/dev/ublkb<id>의 N).
 * @num_queues: io_uring 기반 큐 개수(SMP 분산용).
 * @queue_depth: 큐별 in-flight IO 한도.
 * @ctrl_cb: 비동기 시작 완료 콜백 (커널 ublk 컨트롤 명령 응답 후 호출).
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 비동기 시작 / 음수 errno (bdev 미존재, 이미 사용 중 등).
 */
int ublk_start_disk(const char *bdev_name, uint32_t ublk_id,
		    uint32_t num_queues, uint32_t queue_depth,
		    ublk_ctrl_cb ctrl_cb, void *cb_arg);

/*
 * [한국어]
 * ublk_stop_disk - 특정 ublk 디스크를 중지하고 자원 해제.
 *
 * @ublk_id: 대상 디스크의 ublk ID.
 * @ctrl_cb: 비동기 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 / 음수 errno (해당 ID 없음 등).
 */
int ublk_stop_disk(uint32_t ublk_id, ublk_ctrl_cb ctrl_cb, void *cb_arg);

/*
 * [한국어]
 * ublk_start_disk_recovery - SPDK target 프로세스 재시작 등으로 끊겼던 ublk 디스크를
 *                             기존 ublk 컨트롤 상태(/dev/ublkcN, ublk-id)를 보존한 채
 *                             다시 연결(=커널 ublk가 보유한 in-flight IO를 회수해 SPDK가 재처리).
 *
 * @bdev_name: 다시 연결할 bdev.
 * @ublk_id: 기존 ublk ID(이미 커널에 등록되어 있어야 함).
 * @ctrl_cb: 비동기 완료 콜백.
 * @cb_arg: 콜백 컨텍스트.
 * @return: 0 / 음수 errno.
 *
 * 사용 시나리오: SPDK가 갑자기 재시작되어도 ublk 클라이언트(/dev/ublkb의 사용자)는 단절감 없이
 *  IO를 이어갈 수 있도록 하는 fast-failover 메커니즘.
 */
int
ublk_start_disk_recovery(const char *bdev_name, uint32_t ublk_id, ublk_ctrl_cb ctrl_cb,
			 void *cb_arg);

/*
 * [한국어]
 * ublk_dev_find_by_id - ublk_id로 디스크 객체를 검색.
 *
 * @ublk_id: 찾을 ID.
 * @return: 발견된 디스크 핸들 / NULL.
 */
struct spdk_ublk_dev *ublk_dev_find_by_id(uint32_t ublk_id);

/*
 * [한국어]
 * ublk_dev_get_id - 디스크 핸들에서 ublk_id 추출.
 *
 * @ublk: 디스크 핸들.
 * @return: ublk_id.
 */
uint32_t ublk_dev_get_id(struct spdk_ublk_dev *ublk);

/*
 * [한국어]
 * ublk_dev_get_bdev_name - 이 ublk 디스크 뒤의 bdev 이름.
 *
 * @ublk: 디스크 핸들.
 * @return: bdev 이름 문자열(소유권은 ublk 모듈, 호출자는 free 금지).
 */
const char *ublk_dev_get_bdev_name(struct spdk_ublk_dev *ublk);

/*
 * [한국어]
 * ublk_dev_first/next - 등록된 ublk 디스크들을 enumeration.
 *  RPC `ublk_get_disks`가 전체 디스크 정보를 덤프할 때 사용.
 *
 * @return: 첫/다음 디스크 핸들 / NULL(끝).
 */
struct spdk_ublk_dev *ublk_dev_first(void);
struct spdk_ublk_dev *ublk_dev_next(struct spdk_ublk_dev *prev);

/*
 * [한국어]
 * ublk_dev_get_queue_depth / ublk_dev_get_num_queues - 디스크의 큐 설정 조회.
 *  RPC 응답에 포함되는 메타데이터. */
uint32_t ublk_dev_get_queue_depth(struct spdk_ublk_dev *ublk);
uint32_t ublk_dev_get_num_queues(struct spdk_ublk_dev *ublk);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_UBLK_INTERNAL_H */
