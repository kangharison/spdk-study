/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] NVMe CUSE(Character device in USEr space) 내부 헤더 (nvme_cuse.h)
 *
 * === 파일의 역할 ===
 * SPDK는 유저스페이스 NVMe 드라이버라 커널의 /dev/nvme0 같은 char device가 자동으로
 * 만들어지지 않는다. 이렇게 되면 nvme-cli, smartctl 등 표준 NVMe 관리 툴이 동작할 수
 * 없는데, 이를 해결하기 위해 SPDK는 FUSE-기반 CUSE(Character device in USEr space)
 * 라이브러리(libfuse)를 사용해 /dev/spdk/nvmeX 가짜 char device를 노출한다.
 * 이 헤더는 lib/nvme 내부에서 그 CUSE 게이트웨이를 등록·해제하기 위한 단 두 함수
 * (nvme_cuse_register / nvme_cuse_unregister)를 선언한다. 사용자의 ioctl(IDENTIFY,
 * GET_LOG_PAGE 등)은 CUSE → SPDK admin queue로 포워딩된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   nvme-cli ioctl(/dev/spdk/nvmeX) [커널 VFS]
 *     → libfuse(CUSE) 콜백 [SPDK 프로세스 내 별도 thread]
 *       → nvme_io_msg 큐로 마샬링
 *         → 본 SPDK NVMe 드라이버 thread가 admin command 발행
 *           → CQE 완료 후 응답을 ioctl 결과로 복귀.
 * 본 헤더는 nvme_ctrlr.c가 컨트롤러 attach/detach 시 register/unregister를 호출하기
 * 위한 진입점만 제공한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: libfuse (CUSE), spdk/nvme.h, nvme_io_msg.h.
 * - 의존받음: lib/nvme/nvme_ctrlr.c (옵션이 켜져 있을 때만).
 * - 데이터 흐름: 사용자 ioctl 페이로드 → CUSE worker thread → io_msg ring →
 *   NVMe driver thread → admin SQ doorbell → CQE → ioctl 응답.
 * - 컴파일 가드: SPDK_CONFIG_NVME_CUSE 가 정의되어야 빌드됨.
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_cuse_register   : 컨트롤러를 위한 CUSE char device(/dev/spdk/nvmeX 등) 생성·등록.
 * - nvme_cuse_unregister : 등록된 CUSE char device 해제.
 * 외부 자료구조 노출 없음 — 내부 ctx는 nvme_cuse.c에 캡슐화.
 */

#ifndef __NVME_CUSE_H__
#define __NVME_CUSE_H__
/* [한국어] 다중 포함 가드. */

#include "spdk/nvme.h"
/* [한국어] spdk_nvme_ctrlr 타입 사용 — 컨트롤러 핸들이 등록 키. */

/*
 * [한국어]
 * nvme_cuse_register - 주어진 NVMe 컨트롤러에 대해 CUSE char device를 생성·등록.
 *
 * @ctrlr   : SPDK NVMe 드라이버가 attach한 컨트롤러 핸들. 등록 후에도 유효해야 함.
 * @dev_path: CUSE가 노출할 디바이스 경로 (예: "/dev/spdk/nvme0"). 디렉토리 권한 필요.
 * @return  : 0 성공, 음수 errno 실패 (-EEXIST 중복, -ENOMEM, -EACCES 등).
 *
 * 내부적으로 libfuse cuse_lowlevel_new()로 char device를 띄우고, ioctl 콜백에서
 * nvme_io_msg_send()를 통해 NVMe 드라이버 thread에 admin command 실행을 위임한다.
 * 실행 컨텍스트: NVMe attach 시점(보통 spdk_nvme_probe 콜백 또는 RPC).
 *
 * 호출 체인:
 *   spdk_nvme_cuse_register (공개 API) → [nvme_cuse_register] → libfuse cuse_lowlevel_new
 */
int nvme_cuse_register(struct spdk_nvme_ctrlr *ctrlr, const char *dev_path);

/*
 * [한국어]
 * nvme_cuse_unregister - 등록된 CUSE char device 해제.
 *
 * @ctrlr: 등록 시 사용한 컨트롤러 핸들.
 *
 * detach 또는 컨트롤러 제거 직전에 호출되어 CUSE 워커를 종료시키고 디바이스를 unlink한다.
 * 호출 후에는 사용자가 /dev/spdk/nvmeX에 접근하면 -ENOENT 또는 EIO를 받게 된다.
 *
 * 호출 체인:
 *   spdk_nvme_cuse_unregister → [nvme_cuse_unregister] → libfuse fuse_session_destroy
 */
void nvme_cuse_unregister(struct spdk_nvme_ctrlr *ctrlr);

#endif /* __NVME_CUSE_H__ */
/* [한국어] 다중 포함 가드 종결. */
