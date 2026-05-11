/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] IDXD/DSA 커널 드라이버 경유 backend (idxd_kernel.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK가 Intel DSA(Data Streaming Accelerator)를 *커널 드라이버(idxd.ko)*가 노출한
 * /dev/dsa 캐릭터 디바이스를 통해 사용할 때의 backend 구현이다. 사용자가 미리 accel-config
 * 유틸리티로 디바이스/그룹/WQ를 설정·enable해 둔 상태에서, SPDK는 libaccel-config(accfg_*)
 * API로 enabled WQ를 찾고 해당 WQ의 portal(/dev/char/<major>:<minor>)을 mmap하여
 * 디스크립터를 enqueue한다. 두 backend(user/kernel) 중 kernel 경로는 IOMMU/PASID(SVM)와
 * accel-config 정책을 활용할 수 있어 멀티-프로세스 안전성이 더 높다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   spdk_idxd_probe()
 *     → idxd_impl_register()로 등록된 g_kernel_idxd_impl
 *     → kernel_idxd_probe() (이 파일)
 *         · accfg_new() / accfg_device_foreach() / accfg_wq_foreach()로 enable된 WQ 탐색
 *         · open("/dev/char/<major>:<minor>") + mmap(0x1000, PROT_WRITE, MAP_SHARED)
 *         · attach_cb(spdk_idxd_device)로 SPDK accel framework에 등록
 *     → 이후 idxd.c가 movdir64b(portal, desc)로 디스크립터 enqueue
 * 실행 컨텍스트: probe()는 SPDK init 단계의 mgmt 스레드 1회 호출. destruct는 마지막 채널 닫힌 뒤.
 * portal_get_addr는 채널 생성 시 호출되며, 이후 portal 주소는 read-only.
 *
 * === 타 모듈과의 연결 ===
 * - idxd_internal.h        : spdk_idxd_device, spdk_idxd_impl 등 SPDK 측 구조 정의.
 * - spdk_internal/idxd.h   : SPDK_IDXD_IMPL_REGISTER 매크로 등.
 * - libaccel-config(accfg_*)  : 커널 idxd.ko가 sysfs로 노출한 디바이스/WQ 메타데이터를 읽는 라이브러리.
 * - /dev/char/<major>:<minor> : DSA WQ의 4 KiB portal 페이지 (cdev). mmap하면 user→HW MMIO write가 가능.
 * - kernel idxd.ko          : 실제 PCI 디바이스 핸들링/IOMMU 매핑/그룹 분배.
 * 데이터 흐름:
 *   accel-config(YAML) → sysfs 노출 → libaccel_config 파싱 → kernel_idxd_probe → portal mmap
 *   → idxd.c: 디스크립터 채움 → MOVDIR64B → DSA HW.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_kernel_idxd_device : SPDK 공통 spdk_idxd_device를 컨테이너로 래핑한 backend-specific 구조.
 * - kernel_idxd_probe()           : enable된 모든 DSA WQ를 enumerate, portal mmap 후 attach.
 * - kernel_idxd_device_destruct() : portal munmap + fd close + accfg_unref + free.
 * - kernel_idxd_dump_sw_error()   : 현재 stub (TODO).
 * - kernel_idxd_portal_get_addr() : portal 주소 반환자.
 * - g_kernel_idxd_impl            : impl 콜백 테이블 (probe/destruct/dump_sw_error/portal_get_addr).
 * - SPDK_IDXD_IMPL_REGISTER       : constructor로 g_idxd_impls에 자동 등록.
 */

/* [한국어] SPDK 표준 C 라이브러리 - stdint/stdbool/string/unistd 등을 묶어 헤더 순서 의존성 제거. */
#include "spdk/stdinc.h"

/* [한국어] libaccel-config 헤더 - 커널 idxd.ko가 sysfs로 노출한 디바이스/WQ 정보를
 * 사용자 공간에서 읽기 위한 라이브러리. accfg_ctx, accfg_device, accfg_wq 등을 제공. */
#include <accel-config/libaccel_config.h>

/* [한국어] DPDK/SPDK 환경 추상화 (hugepage, NUMA, IOMMU 상태 조회 등). */
#include "spdk/env.h"
/* [한국어] 일반 유틸리티 (SPDK_CONTAINEROF 매크로 등). */
#include "spdk/util.h"
/* [한국어] 메모리 관련 헬퍼(여기서는 직접 사용은 적지만, 의존 헤더로 포함). */
#include "spdk/memory.h"
/* [한국어] spdk_likely/spdk_unlikely - 분기 예측 힌트(이 파일에선 직접 사용은 없음, 가독성/일관성). */
#include "spdk/likely.h"

/* [한국어] SPDK 로깅(SPDK_ERRLOG/INFOLOG 등). */
#include "spdk/log.h"
/* [한국어] SPDK 내부 IDXD 헤더 - SPDK_IDXD_IMPL_REGISTER 매크로 정의 위치. */
#include "spdk_internal/idxd.h"

/* [한국어] 같은 디렉토리 내부 헤더 - spdk_idxd_device/spdk_idxd_impl/idxd_ops 등 backend 공통 구조. */
#include "idxd_internal.h"

/* [한국어] kernel backend 전용 디바이스 구조체 - SPDK 공통 spdk_idxd_device를 첫 멤버로 임베드.
 * SPDK_CONTAINEROF로 공통 구조체 ↔ backend 구조체를 양방향 변환한다. */
struct spdk_kernel_idxd_device {
	struct spdk_idxd_device	idxd;
	/* [한국어] SPDK 공통 IDXD 디바이스 구조체 - impl/portal/socket_id 등 framework가 보는 view.
	 * 설정자: kernel_idxd_probe()가 accfg_*에서 읽은 값으로 채움.
	 * 읽는 자: SPDK accel framework, idxd.c의 일반 코드.
	 * 동기화: 초기화 후 대부분 read-only, num_channels만 mutex로 보호. */

	struct accfg_ctx	*ctx;
	/* [한국어] libaccel-config의 컨텍스트 객체 - sysfs 정보 캐시 + reference counting.
	 * 설정자: probe 시 accfg_new()로 1개 만들어 모든 디바이스가 ref 증가하며 공유.
	 * 읽는 자: destruct 시 accfg_unref()로 reference 감소.
	 * 값 범위: 유효한 ctx 포인터 (NULL 불가).
	 * 동기화: libaccel-config 내부 ref count는 atomic. */

	unsigned int		max_batch_size;
	/* [한국어] 디바이스가 1 batch에 허용하는 최대 inner descriptor 수.
	 * 설정자: probe 시 accfg_device_get_max_batch_size()로 획득.
	 * 읽는 자: SPDK 측 batch_size 결정 시 참조 (실제로는 wq 단위로 다시 결정).
	 * 값 범위: DSA spec 의존 (보통 16, 64, 128, 256 …). */

	unsigned int		max_xfer_size;
	/* [한국어] 단일 디스크립터 1개의 최대 전송 바이트.
	 * 설정자: probe 시 accfg_device_get_max_transfer_size()로 획득.
	 * 읽는 자: idxd.c의 split 로직이 사용자 요청을 잘게 자를 때 참조.
	 * 값 범위: 디바이스 GENCAP의 max xfer (보통 2 GiB). */

	unsigned int		max_xfer_bits;
	/* [한국어] max_xfer_size를 표현하는 비트 수 (log2). 현재 코드에서 직접 set은 없지만
	 * 향후 split 시 비트시프트 용도로 예약된 멤버. */

	/* We only use a single WQ */
	struct accfg_wq		*wq;
	/* [한국어] SPDK가 이 디바이스에서 채택한 단일 WQ 핸들.
	 * 설정자: probe 시 첫 번째 ENABLED && USER && DEDICATED WQ를 선택.
	 * 읽는 자: 추후 wq 메타데이터 추출 시 사용 (현재는 size만 사용).
	 * 값 범위: 유효한 WQ 포인터.
	 * 동기화: 초기화 후 read-only. */

	int			fd;
	/* [한국어] WQ의 cdev(/dev/char/<M>:<m>)를 open한 파일 디스크립터.
	 * 설정자: probe 시 open()으로 획득.
	 * 읽는 자: destruct 시 close().
	 * 값 범위: -1(미초기화) 또는 0 이상의 fd. */

	void			*portal;
	/* [한국어] WQ portal(4 KiB MMIO 페이지)을 mmap한 가상주소 - 디스크립터 enqueue 대상.
	 * 설정자: mmap(NULL, 0x1000, PROT_WRITE, MAP_SHARED|MAP_POPULATE, fd, 0).
	 * 읽는 자: idxd.c가 movdir64b(portal, desc)로 디스크립터 전송 시 사용.
	 * 값 범위: NULL/MAP_FAILED(미초기화/실패) 또는 4 KiB 정렬된 mmap 주소.
	 * 동기화: WRITE-ONLY MMIO, kernel 드라이버가 PASID/IOMMU 처리. */
};

/* [한국어] 공통 spdk_idxd_device → 이 backend의 spdk_kernel_idxd_device로 다운캐스트하는 매크로.
 * spdk_idxd_device가 첫 멤버로 임베드되어 있으므로 SPDK_CONTAINEROF로 안전하게 변환 가능. */
#define __kernel_idxd(idxd) SPDK_CONTAINEROF(idxd, struct spdk_kernel_idxd_device, idxd)

/*
 * [한국어]
 * kernel_idxd_device_destruct - kernel backend 디바이스 1개를 해제
 *
 * @idxd: 해제 대상 SPDK 공통 디바이스 포인터 (실제로는 spdk_kernel_idxd_device의 임베드 멤버).
 * @return: 없음 (void).
 *
 * 왜 필요한가: 마지막 채널이 닫히거나 SPDK shutdown 시 portal mmap, cdev fd, accfg_ctx 참조,
 *   디바이스 자체 구조체 메모리를 모두 해제해 자원 누수와 dangling FD를 방지한다.
 *
 * 동작 과정:
 *   1) portal이 매핑되어 있으면 munmap(0x1000) → 페이지 테이블에서 해제.
 *   2) fd >= 0이면 close() → 커널 드라이버가 WQ 참조 카운트 감소.
 *   3) accfg_unref(ctx) → libaccel-config의 디바이스별 ref 감소 (probe에서 accfg_ref한 짝).
 *   4) free(kernel_idxd) → calloc으로 잡았던 backend 구조체 자체 해제.
 *
 * 실행 컨텍스트: SPDK 종료 또는 동적 detach 경로 - 단일 스레드.
 *
 * 호출 체인:
 *   spdk_idxd_detach() → impl->destruct() → [kernel_idxd_device_destruct]
 */
static void
kernel_idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	/* [한국어] 공통 구조체 → backend 전용 구조체로 다운캐스트. */
	struct spdk_kernel_idxd_device *kernel_idxd = __kernel_idxd(idxd);

	/* [한국어] portal이 mmap되어 있으면 해제 - mmap이 실패한 경우 NULL일 수 있어 가드. */
	if (kernel_idxd->portal != NULL) {
		/* [한국어] 4 KiB(0x1000)만 매핑했으므로 동일 길이로 munmap. */
		munmap(kernel_idxd->portal, 0x1000);
	}

	/* [한국어] cdev fd가 유효(0 이상)하면 close - kernel idxd.ko가 WQ user count 감소. */
	if (kernel_idxd->fd >= 0) {
		close(kernel_idxd->fd);
	}

	/* [한국어] probe에서 accfg_ref()로 잡았던 ctx ref를 해제 - 마지막 ref면 ctx 메모리도 해제됨. */
	accfg_unref(kernel_idxd->ctx);
	/* [한국어] backend 전용 구조체 자체 해제 - calloc 파트너. */
	free(kernel_idxd);
}

/* [한국어] 전방 선언 - 정적 g_kernel_idxd_impl 구조체. 아래에서 멤버를 채워 정의. */
static struct spdk_idxd_impl g_kernel_idxd_impl;

/*
 * [한국어]
 * kernel_idxd_probe - 시스템의 모든 enable된 DSA WQ를 enumerate하고 SPDK에 attach
 *
 * @cb_ctx: 사용자가 spdk_idxd_probe()에 전달한 컨텍스트 - attach_cb로 그대로 전달.
 * @attach_cb: 사용 가능 디바이스 발견 시 호출 - 사용자가 attach 여부 결정 (return value 없음).
 * @probe_cb: 사전 필터링 콜백 (이 파일에선 사용 안 함 - libaccel-config의 메타데이터로 충분).
 * @return: 0=성공, 음수=errno (-ENOMEM, -ENOTSUP 등).
 *
 * 왜 필요한가: SPDK가 어떤 DSA 디바이스를 사용할지는 런타임에 결정한다. accel-config로 미리
 * 그룹/WQ를 만들고 enable한 사용자만이 SPDK 가속 대상이 된다. 이 함수는 그 규약을 enforce한다.
 *
 * 동작 과정:
 *   1) accfg_new()로 libaccel-config 컨텍스트 생성 (sysfs 스캔).
 *   2) accfg_device_foreach()로 각 DSA 디바이스 순회.
 *      - state != ENABLED면 skip.
 *      - PASID 비활성 + SPDK IOMMU 활성 조합은 사용 불가 (-ENOTSUP).
 *      - backend 구조체 calloc + 메타데이터(max_batch_size, NUMA, version 등) 채움.
 *      - accfg_wq_foreach로 디바이스 내 WQ 순회:
 *          · ENABLED && USER 타입 && DEDICATED 모드인 첫 WQ 선택.
 *          · cdev major:minor로 /dev/char/<M>:<m> open.
 *          · portal 4 KiB mmap (PROT_WRITE, MAP_SHARED|MAP_POPULATE).
 *          · 단일 WQ만 사용하므로 첫 성공 시 break.
 *   3) WQ를 하나라도 잡았으면 attach_cb 호출, 아니면 destruct로 정리.
 *   4) accfg_unref(ctx)로 함수 진입 시 만든 임시 ref 해제 (각 디바이스가 자기 ref를 별도로 잡음).
 *
 * 실행 컨텍스트: spdk_idxd_probe() 호출 스레드 (보통 application init 단계, 단일 스레드).
 *
 * 에러 처리:
 *   - calloc 실패 시 -ENOMEM 즉시 return (TODO: 부분적으로 잡힌 디바이스 정리 미흡).
 *   - WQ open/mmap 실패는 continue로 다음 WQ 시도, 모든 WQ 실패 시 디바이스 destruct.
 *
 * 호출 체인:
 *   spdk_idxd_probe() → impl->probe() → [kernel_idxd_probe]
 *     → libaccel_config (accfg_*) → kernel idxd.ko sysfs
 *     → 사용자 attach_cb (spdk_idxd_device 등록 결정)
 */
static int
kernel_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb, spdk_idxd_probe_cb probe_cb)
{
	int rc;                          /* [한국어] accfg_new 등의 반환값 임시 저장. */
	struct accfg_ctx *ctx;           /* [한국어] libaccel-config 마스터 컨텍스트(sysfs 캐시). */
	struct accfg_device *device;     /* [한국어] foreach 순회 변수 - 디바이스 1개. */

	/* Create a configuration context, incrementing the reference count. */
	/* [한국어] accfg_new는 sysfs를 스캔해 디바이스 트리를 만들고 ref count=1로 설정. */
	rc = accfg_new(&ctx);
	if (rc < 0) {
		/* [한국어] sysfs 접근 실패(권한 부족, accel-config 미설치 등) - 가속기 사용 불가. */
		SPDK_ERRLOG("Unable to allocate accel-config context\n");
		return rc;
	}

	/* Loop over each IDXD device */
	/* [한국어] /sys/bus/dsa/devices/dsa* 각각에 대해 1회 호출. */
	accfg_device_foreach(ctx, device) {
		enum accfg_device_state dstate;            /* [한국어] 디바이스 enable 상태. */
		struct spdk_kernel_idxd_device *kernel_idxd; /* [한국어] backend 전용 구조체 포인터. */
		struct accfg_wq *wq;                       /* [한국어] WQ 순회 변수. */
		bool pasid_enabled;                        /* [한국어] PASID(SVM) 지원 플래그. */

		/* Make sure that the device is enabled */
		/* [한국어] accel-config로 enable되지 않은 디바이스는 사용 불가 - 사용자가 활성화해야 함. */
		dstate = accfg_device_get_state(device);
		if (dstate != ACCFG_DEVICE_ENABLED) {
			continue;  /* [한국어] 다음 디바이스로. */
		}

		/* [한국어] PASID(=SVM, Shared Virtual Memory) 활성 여부 - 호스트 가상주소를 직접 IOVA로 사용 가능한지. */
		pasid_enabled = accfg_device_get_pasid_enabled(device);
		/* [한국어] SPDK가 IOMMU 활성 모드인데 PASID가 비활성이면, 호스트 가상주소→IOVA 변환 경로가 없어
		 * 디바이스가 우리 메모리에 접근할 수 없다. SPDK는 이 조합을 명시적으로 거부. */
		if (!pasid_enabled && spdk_iommu_is_enabled()) {
			/*
			 * If the IOMMU is enabled but shared memory mode is not on,
			 * then we have no way to get the IOVA from userspace to use this
			 * device or any kernel device. Return an error.
			 */
			SPDK_ERRLOG("Found kernel IDXD device, but cannot use it when IOMMU is enabled but SM is disabled\n");
			return -ENOTSUP;  /* [한국어] 다른 디바이스도 같은 문제이므로 즉시 반환. */
		}

		/* [한국어] backend 전용 디바이스 구조체 1개 할당 (zero-init). */
		kernel_idxd = calloc(1, sizeof(struct spdk_kernel_idxd_device));
		if (kernel_idxd == NULL) {
			SPDK_ERRLOG("Failed to allocate memory for kernel_idxd device.\n");
			/* TODO: Goto error cleanup */
			/* [한국어] TODO: 이미 attach된 다른 디바이스에 대한 cleanup 누락 - 향후 개선 필요. */
			return -ENOMEM;
		}

		/* [한국어] 디바이스 capability를 backend 구조체에 캐시. */
		kernel_idxd->max_batch_size = accfg_device_get_max_batch_size(device);
		kernel_idxd->max_xfer_size = accfg_device_get_max_transfer_size(device);
		/* [한국어] 디바이스가 위치한 NUMA 노드 - SPDK가 같은 NUMA의 코어에 채널 배치. */
		kernel_idxd->idxd.socket_id = accfg_device_get_numa_node(device);
		/* [한국어] backend impl 포인터 설정 - 이후 destruct/portal_get_addr 디스패치에 사용. */
		kernel_idxd->idxd.impl = &g_kernel_idxd_impl;
		/* [한국어] fd를 -1로 초기화 - mmap 실패 시 destruct에서 close 가드용. */
		kernel_idxd->fd = -1;
		/* [한국어] 디바이스 세대(version) 캐시 - 일부 capability 분기에 사용 가능. */
		kernel_idxd->idxd.version = accfg_device_get_version(device);
		/* [한국어] PASID 사용 가능 여부 저장 - 채널 생성 시 io_channel.pasid_enabled로 복사됨. */
		kernel_idxd->idxd.pasid_enabled = pasid_enabled;

		/* Increment configuration context reference for each device. */
		/* [한국어] 각 디바이스가 자기 ref를 잡음 - destruct에서 accfg_unref 짝.
		 * 주의: kernel_idxd->ctx는 calloc으로 NULL 상태이므로 accfg_ref(NULL) 결과는 ctx 변수의 새 ref. */
		kernel_idxd->ctx = accfg_ref(kernel_idxd->ctx);

		/* [한국어] 이 디바이스 내의 모든 WQ 순회 - 첫 번째로 사용 가능한 WQ만 채택. */
		accfg_wq_foreach(device, wq) {
			enum accfg_wq_state wstate;  /* [한국어] WQ enable 상태. */
			enum accfg_wq_mode mode;     /* [한국어] DEDICATED vs SHARED. */
			enum accfg_wq_type type;     /* [한국어] USER(유저스페이스) vs KERNEL. */
			int major, minor;            /* [한국어] cdev 번호 - /dev/char/<M>:<m> 경로 구성용. */
			char path[1024];             /* [한국어] cdev 경로 문자열 버퍼. */

			/* [한국어] WQ가 enable되어 있어야 사용 가능 - 그렇지 않으면 다음 WQ로. */
			wstate = accfg_wq_get_state(wq);
			if (wstate != ACCFG_WQ_ENABLED) {
				continue;
			}

			/* [한국어] USER 타입 WQ만 사용 가능 - KERNEL 타입은 in-kernel offload 전용. */
			type = accfg_wq_get_type(wq);
			if (type != ACCFG_WQT_USER) {
				continue;
			}

			/* TODO: For now, only support dedicated WQ */
			/* [한국어] DEDICATED 모드만 지원: 단일 producer 보장 → MOVDIR64B 사용 가능.
			 * SHARED 모드는 ENQCMDS 명령(retry 가능)을 써야 하는데 SPDK는 아직 미구현. */
			mode = accfg_wq_get_mode(wq);
			if (mode != ACCFG_WQ_DEDICATED) {
				continue;
			}

			/* [한국어] 디바이스의 cdev major(공통) 획득 - 음수면 cdev 미생성된 상태. */
			major = accfg_device_get_cdev_major(device);
			if (major < 0) {
				continue;
			}

			/* [한국어] WQ별 cdev minor 획득 - 음수면 이 WQ의 cdev가 없음. */
			minor = accfg_wq_get_cdev_minor(wq);
			if (minor < 0) {
				continue;
			}

			/* Map the portal */
			/* [한국어] /dev/char/<major>:<minor> 경로 문자열 구성 - 커널이 udev 규칙으로 생성. */
			snprintf(path, sizeof(path), "/dev/char/%u:%u", major, minor);
			/* [한국어] cdev open(읽기쓰기) - 커널 idxd.ko가 PASID 등록과 WQ 매핑 권한 확인. */
			kernel_idxd->fd = open(path, O_RDWR);
			if (kernel_idxd->fd < 0) {
				SPDK_ERRLOG("Can not open the WQ file descriptor on path=%s\n",
					    path);
				continue;
			}

			/* [한국어] portal 4 KiB MMIO 페이지 mmap.
			 * - PROT_WRITE만 부여(읽기 불필요): WQ portal은 write-only MMIO.
			 * - MAP_SHARED: 커널과 매핑 공유.
			 * - MAP_POPULATE: 페이지 테이블을 즉시 채워 첫 access의 minor fault 회피 (latency 안정화).
			 * - offset 0: WQ portal은 cdev offset 0에 단일 페이지로 노출. */
			kernel_idxd->portal = mmap(NULL, 0x1000, PROT_WRITE,
						   MAP_SHARED | MAP_POPULATE, kernel_idxd->fd, 0);
			if (kernel_idxd->portal == MAP_FAILED) {
				/* [한국어] EPERM은 보통 CAP_SYS_RAWIO 부족 - 사용자가 root이거나 해당 capability 필요. */
				if (errno == EPERM) {
					SPDK_ERRLOG("CAP_SYS_RAWIO capabilities required to mmap the portal\n");
				}
				perror("mmap");
				continue;  /* [한국어] 다음 WQ 시도 (이 WQ의 fd는 close되지 않음 - destruct에서 일괄 처리). */
			}

			/* [한국어] 채택한 WQ 핸들 저장 - 이후 size/batch_size 추출에 사용. */
			kernel_idxd->wq = wq;

			/* Since we only use a single WQ, the total size is the size of this WQ */
			/* [한국어] 단일 WQ 사용 정책이므로 total_wq_size = 이 WQ의 크기. */
			kernel_idxd->idxd.total_wq_size = accfg_wq_get_size(wq);
			/* [한국어] WQ 크기에 따라 채널 수 결정: 큰 WQ(>=128)면 8개, 작으면 4개로 분할.
			 * 채널이 많을수록 SPDK 스레드별 병렬성↑, 하지만 채널당 in-flight 한도는 줄어듦. */
			kernel_idxd->idxd.chan_per_device = (kernel_idxd->idxd.total_wq_size >= 128) ? 8 : 4;

			/* [한국어] 1 batch에 들어갈 수 있는 inner descriptor의 최대 개수 - WQ별로 다를 수 있음. */
			kernel_idxd->idxd.batch_size = accfg_wq_get_max_batch_size(wq);

			/* We only use a single WQ, so once we've found one we can stop looking. */
			/* [한국어] 첫 사용 가능 WQ 채택 후 즉시 break - 한 디바이스당 단일 WQ 정책. */
			break;
		}

		/* [한국어] WQ 1개 이상 잡혔으면 attach_cb로 SPDK 사용자에게 알림, 아니면 정리. */
		if (kernel_idxd->idxd.total_wq_size > 0) {
			/* This device has at least 1 WQ available, so ask the user if they want to use it. */
			/* [한국어] attach_cb는 보통 SPDK accel framework 내부의 등록 콜백 - 디바이스를 풀에 추가. */
			attach_cb(cb_ctx, &kernel_idxd->idxd);
		} else {
			/* [한국어] 사용 가능 WQ가 0개면 이 디바이스 무용 - 즉시 자원 해제. */
			kernel_idxd_device_destruct(&kernel_idxd->idxd);
		}
	}

	/* Release the reference used for configuration. */
	/* [한국어] 함수 진입 시 accfg_new로 잡은 임시 ctx ref 해제 - 각 디바이스는 자기 ref를 별도로 보유. */
	accfg_unref(ctx);

	return 0;  /* [한국어] enumerate 자체는 항상 성공 - 사용 가능한 디바이스가 0개여도 OK. */
}

/*
 * [한국어]
 * kernel_idxd_dump_sw_error - SW 에러 상태 덤프 (현재 stub)
 *
 * @idxd: 대상 디바이스.
 * @portal: 에러를 일으킨 portal 주소 (디버깅 컨텍스트).
 * @return: 없음.
 *
 * 왜 stub인가: kernel backend는 idxd.ko가 SWERROR 레지스터 처리를 담당하므로 사용자 공간에서
 * 직접 SWERROR을 읽기보다 dmesg를 보는 편이 일반적. 향후 ioctl 등으로 가져올 수 있도록 자리만 둠.
 *
 * 호출 체인:
 *   idxd.c가 process_events에서 비정상 발견 → impl->dump_sw_error → [kernel_idxd_dump_sw_error]
 */
static void
kernel_idxd_dump_sw_error(struct spdk_idxd_device *idxd, void *portal)
{
	/* Need to be enhanced later */
	/* [한국어] 현재는 빈 함수 - kernel idxd.ko가 dmesg로 SWERROR을 출력하므로 SPDK 측 추가 처리 없음. */
}

/*
 * [한국어]
 * kernel_idxd_portal_get_addr - 이 디바이스의 portal 주소 반환
 *
 * @idxd: SPDK 공통 디바이스 포인터.
 * @return: portal mmap 주소 (char * 캐스팅).
 *
 * 왜 필요한가: SPDK 공통 코드(idxd.c)는 backend 구조체를 모르므로 impl 콜백을 통해 portal을 얻어야 한다.
 *   채널 생성 시 1회 호출되어 io_channel.portal에 저장된다.
 *
 * 호출 체인:
 *   spdk_idxd_get_channel() → impl->portal_get_addr → [kernel_idxd_portal_get_addr]
 */
static char *
kernel_idxd_portal_get_addr(struct spdk_idxd_device *idxd)
{
	/* [한국어] 공통 → backend 다운캐스트 후 portal 그대로 반환. */
	struct spdk_kernel_idxd_device *kernel_idxd = __kernel_idxd(idxd);

	return kernel_idxd->portal;
}

/* [한국어] kernel backend의 impl 콜백 테이블 - SPDK_IDXD_IMPL_REGISTER가 등록 시 이 구조체 포인터를 사용.
 * 모든 콜백이 정적 함수 이름과 매칭되어 컴파일 타임 안전성 확보. */
static struct spdk_idxd_impl g_kernel_idxd_impl = {
	.name			= "kernel",                  /* [한국어] backend 식별 문자열 - RPC에서 "kernel" 지정 시 매칭. */
	.probe			= kernel_idxd_probe,         /* [한국어] 디바이스 enumerate. */
	.destruct		= kernel_idxd_device_destruct, /* [한국어] 디바이스 해제. */
	.dump_sw_error		= kernel_idxd_dump_sw_error,   /* [한국어] 디버깅 stub. */
	.portal_get_addr	= kernel_idxd_portal_get_addr, /* [한국어] portal 주소 반환자. */
};

/* [한국어] constructor 속성으로 main() 진입 전에 idxd_impl_register(&g_kernel_idxd_impl) 자동 호출.
 * 결과: SPDK init 시 spdk_idxd_probe()가 g_idxd_impls 리스트에서 이 backend의 probe()도 호출하게 됨. */
SPDK_IDXD_IMPL_REGISTER(kernel, &g_kernel_idxd_impl);
