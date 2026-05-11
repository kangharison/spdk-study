/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] NBD(Network Block Device) 내부 헬퍼 헤더 (nbd_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK NBD 모듈이 RPC 핸들러(lib/nbd/nbd_rpc.c)와 코어 구현(lib/nbd/nbd.c)
 * 사이에서 공유해야 하는 "내부" API만을 모은 헤더이다. 공개 API
 * (spdk_nbd_start, spdk_nbd_stop 등)는 include/spdk/nbd.h에 있고, 본 헤더는
 * 검색/순회/필드 getter/연결 해제 같은 RPC가 응답을 만들 때 필요한 작은 함수만 노출한다.
 * NBD는 Linux 커널의 nbd 모듈(/dev/nbdN)이 사용자 공간 서버(SPDK)에 블록 I/O를
 * NBD 프로토콜(소켓 기반)로 위임하는 시스템이며, SPDK는 그 서버 측을 구현해
 * SPDK bdev를 /dev/nbdN으로 노출시킨다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   사용자 mkfs/dd /dev/nbd0
 *     → 커널 NBD 클라이언트(/dev/nbd0)
 *       → AF_UNIX socketpair (kernel side fd ↔ SPDK side fd)
 *         → SPDK NBD server(lib/nbd/nbd.c)
 *           → spdk_bdev_read/write
 *             → bdev module (nvme/aio/...)
 * 본 헤더의 함수들은 RPC "nbd_get_disks", "nbd_stop_disk" 등이 nbd_disk 리스트를
 * 순회하거나 특정 디스크를 검색해 disconnect할 때 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/nbd.h(struct spdk_nbd_disk 전방선언), spdk/stdinc.h.
 * - 의존받음: lib/nbd/nbd_rpc.c (RPC 응답 빌더가 모든 함수 사용).
 * - 데이터 흐름: nbd 디스크 등록 시 nbd.c의 전역 g_spdk_nbd.disk_head TAILQ에
 *   spdk_nbd_disk 객체가 추가됨 → RPC가 본 헤더 함수들로 walk/lookup.
 * - 공유 자료구조: struct spdk_nbd_disk (nbd.c에 정의, 외부에는 불투명).
 *
 * === 주요 함수/구조체 요약 ===
 * - nbd_disk_find_by_nbd_path : "/dev/nbdN" 경로로 nbd 디스크 검색.
 * - nbd_disk_first/next       : 등록된 모든 nbd 디스크 순회.
 * - nbd_disk_get_nbd_path     : 디스크 → 노출 경로(/dev/nbdN) 추출.
 * - nbd_disk_get_bdev_name    : 디스크 → 백킹 bdev 이름 추출.
 * - nbd_disconnect            : 비동기 disconnect 시작 (RPC nbd_stop_disk가 사용).
 */

#ifndef SPDK_NBD_INTERNAL_H
#define SPDK_NBD_INTERNAL_H
/* [한국어] 다중 포함 가드. */

#include "spdk/stdinc.h" /* [한국어] SPDK 표준 헤더 모음. */
#include "spdk/nbd.h"    /* [한국어] 공개 API + struct spdk_nbd_disk 전방선언. */

/*
 * [한국어]
 * nbd_disk_find_by_nbd_path - "/dev/nbdN" 경로로 등록된 nbd 디스크 검색.
 *
 * @nbd_path: 사용자가 nbd_start_disk RPC에서 지정한 커널 디바이스 경로.
 * @return  : 매치되는 spdk_nbd_disk 포인터 또는 NULL.
 *
 * 호출 체인:
 *   rpc_nbd_stop_disk → [nbd_disk_find_by_nbd_path] → 전역 TAILQ 선형 탐색
 */
struct spdk_nbd_disk *nbd_disk_find_by_nbd_path(const char *nbd_path);

/*
 * [한국어]
 * nbd_disk_first - 등록된 nbd 디스크 리스트의 첫 노드.
 * @return: 첫 디스크 포인터 또는 NULL.
 */
struct spdk_nbd_disk *nbd_disk_first(void);

/*
 * [한국어]
 * nbd_disk_next - 다음 nbd 디스크.
 * @prev  : 이전 노드 포인터(NULL 불가).
 * @return: 다음 또는 NULL.
 */
struct spdk_nbd_disk *nbd_disk_next(struct spdk_nbd_disk *prev);

/*
 * [한국어]
 * nbd_disk_get_nbd_path - 디스크에 매핑된 커널 NBD 디바이스 경로를 반환.
 * @nbd   : 대상 디스크.
 * @return: "/dev/nbdN" 같은 문자열 (디스크가 살아있는 동안 유효).
 */
const char *nbd_disk_get_nbd_path(struct spdk_nbd_disk *nbd);

/*
 * [한국어]
 * nbd_disk_get_bdev_name - 디스크가 백킹으로 사용 중인 SPDK bdev의 이름.
 * @nbd   : 대상 디스크.
 * @return: bdev 이름 문자열.
 */
const char *nbd_disk_get_bdev_name(struct spdk_nbd_disk *nbd);

/*
 * [한국어]
 * nbd_disconnect - nbd 디스크의 비동기 종료를 시작.
 *
 * @nbd: 대상 디스크.
 *
 * 즉시 반환되며, 실제 자원 해제는 nbd 메인 poller가 이후 사이클에서 수행한다.
 * 이는 진행 중인 I/O를 안전하게 마무리하기 위함이다(사용자 fs unmount → kernel
 * NBD client가 NBD_DISCONNECT를 보낼 때까지 대기 후 정리).
 *
 * 호출 체인:
 *   rpc_nbd_stop_disk → [nbd_disconnect] → nbd 디스크 상태 머신을 disconnecting으로 전이
 */
void nbd_disconnect(struct spdk_nbd_disk *nbd);

#endif /* SPDK_NBD_INTERNAL_H */
/* [한국어] 다중 포함 가드 종결. */
