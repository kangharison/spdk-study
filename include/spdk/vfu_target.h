/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] vfio-user 타깃(server) 측 PCI 디바이스 에뮬레이션 추상화 (vfu_target.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 가 vfio-user 프로토콜의 "타깃(server)" 으로 동작할 때, 즉
 * 외부 클라이언트(QEMU VM, 또는 다른 SPDK 프로세스의 nvme_vfio_user 드라이버)에
 * 가상 PCI 디바이스를 노출할 때 사용하는 상위 추상 API 를 정의한다. 실제
 * vfio-user 와이어 메시지 처리는 외부 라이브러리 libvfio-user(서브모듈
 * extern/libvfio-user) 가 담당하고, 본 헤더는 그 위에 SPDK 친화적 콜백
 * 모델(struct spdk_vfu_endpoint_ops 의 vtable)과 endpoint 등록·삭제·관리
 * RPC 진입점을 얹은 thin wrapper 의 인터페이스이다.
 *
 * 본 헤더는 두 가지 사용자가 있다:
 *   1) "백엔드 PCI 디바이스 모듈" — 예: module/vfu_device/vfu_virtio_blk,
 *      module/nvmf/transport/vfio_user 등. 이들은 spdk_vfu_register_endpoint_ops()
 *      를 호출해 자신의 vtable 을 등록하고, 그 vtable 콜백 안에서 실제 NVMe/
 *      virtio 레지스터 시뮬레이션을 구현한다.
 *   2) "프레임워크 사용자/RPC" — spdk_vfu_create_endpoint() / delete_endpoint()
 *      / set_socket_path() 를 호출해 endpoint(=Unix 도메인 소켓 파일)를 만들고
 *      삭제한다. 보통 RPC 핸들러(rpc_create_endpoint 등)에서 호출된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 의 vfio-user 타깃 스택은 클라이언트 스택(vfio_user_pci.h)과 정확히
 * 대칭이다 — client 는 socket 연결 후 메시지를 "보내고", server 는 그 메시지
 * 를 "받아 처리한다". 호출 체인은 다음과 같다:
 *
 *   외부 클라이언트(QEMU/SPDK nvme_vfio_user)
 *     ↔ Unix 도메인 소켓 (vfio-user 와이어 프로토콜 — vfio_user_spec.h)
 *       ↔ libvfio-user (extern/libvfio-user) — 메시지 디코딩/디스패치
 *         ↔ lib/vfu_tgt/tgt_endpoint.c — SPDK 측 endpoint 글루
 *           ↔ struct spdk_vfu_endpoint_ops vtable 콜백 (본 헤더)
 *             ↔ 백엔드 모듈 (module/vfu_device/vfu_virtio_blk.c,
 *               module/vfu_device/vfu_virtio_scsi.c, NVMe-vfio target 등)
 *               ↔ SPDK bdev 레이어 → 실제 NVMe SSD/AIO/lvol
 *
 * 즉 본 헤더는 "라이브러리 레이어(libvfio-user) ↔ 백엔드 디바이스 모듈"
 * 사이의 경계이며, vtable 콜백 패턴으로 양측을 디커플링한다. 백엔드 모듈은
 * spdk_vfu_get_endpoint_private() 로 자기 모듈 데이터를 꺼내고 PCI BAR
 * 접근 콜백(spdk_vfu_access_cb)에서 가상 NVMe 컨트롤러 레지스터(CC/CSTS/AQA/
 * 도어벨 등)를 시뮬레이션한다.
 *
 * 실행 컨텍스트: endpoint 별로 cpumask 가 지정되며(spdk_vfu_create_endpoint
 * 의 cpumask_str), 그 코어들 중 하나의 spdk_thread 에 endpoint 가 핀된다.
 * libvfio-user 의 fd 들이 SPDK poller 로 감시되고, client 메시지 수신 시
 * 해당 thread 에서 vtable 콜백이 동기적으로 실행된다 — 따라서 콜백은
 * non-blocking 이어야 한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존:
 *   * <vfio-user/libvfio-user.h>: extern/libvfio-user 의 공용 헤더 — vfu_ctx_t,
 *     dma_sg_t, vfu_pci_type_t 등 외부 라이브러리 타입을 가져온다.
 *   * <vfio-user/pci_defs.h>: PCI capability 구조체(struct pmcap, pxcap, msixcap)
 *     정의. PCI/PCIe 스펙 표준 capability 헤더의 비트필드 모델.
 *   * <sys/uio.h> (간접): struct iovec, struct iovec ↔ dma_sg_t 매핑에 사용.
 * - 의존자:
 *   * lib/vfu_tgt/tgt_endpoint.c, tgt_rpc.c — 본 헤더의 함수 구현체.
 *   * module/vfu_device/* — virtio-blk/virtio-scsi 백엔드 (vtable 등록자).
 *   * lib/nvmf/vfio_user.c — NVMe-vfio target transport (vtable 등록자이지만
 *     별도 transport API 도 사용).
 *   * app/spdk_tgt — JSON-RPC 핸들러를 통해 endpoint 생성/삭제 명령 노출.
 * - 데이터 흐름: 외부 client 의 BAR write → libvfio-user → tgt_endpoint 의
 *   콜백 디스패치 → 본 헤더의 spdk_vfu_access_cb (vtable 의 PCI region access)
 *   → 백엔드 모듈이 가상 레지스터 업데이트 → bdev_io 발사 → 완료 시
 *   spdk_vfu_map_one() 으로 게스트 메모리에 결과 DMA 후 응답.
 * - 공유 자료구조: 불투명 핸들 struct spdk_vfu_endpoint — 본 헤더에는
 *   forward declaration 만 노출, 내부에는 vfu_ctx_t*, vtable ops 포인터, 백엔드
 *   private 데이터, region 메타, MSI-X 상태 등이 캡슐화되어 있다. 백엔드는
 *   getter 함수들로만 접근.
 *
 * 클라이언트 측 헤더(vfio_user_pci.h)와의 관계: 같은 와이어 프로토콜
 * (vfio_user_spec.h) 위에서 "보내는 측"과 "받는 측"이 거울처럼 대응한다.
 * client 의 spdk_vfio_user_pci_bar_access(write) 1회는 server 측 본 헤더의
 * spdk_vfu_access_cb(is_write=true) 1회 호출로 연결된다. DMA 매핑도
 * client 의 setup() 시 보낸 DMA_MAP 메시지가 server 의 post_memory_add() 로
 * 통보된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_vfu_init / fini: vfio-user 타깃 환경 초기화/종료 — 모듈 로드 시
 *   1회 호출. libvfio-user 의 글로벌 초기화 + SPDK poller 등록 등.
 * - spdk_vfu_register_endpoint_ops: 백엔드 디바이스 타입(이름)을 vtable
 *   과 함께 등록. 등록된 타입 이름은 create_endpoint 의 dev_type_name 으로
 *   참조된다.
 * - spdk_vfu_create_endpoint / delete_endpoint: 실제 Unix 도메인 소켓 파일
 *   하나를 생성/삭제. RPC 와 1:1 대응.
 * - spdk_vfu_set_socket_path: 모든 endpoint 의 소켓이 만들어지는 base
 *   디렉토리 설정 (예: /var/tmp).
 * - spdk_vfu_endpoint getter 들: get_endpoint_id/name/private/vfu_ctx,
 *   msix_enabled/intx_enabled, get_pci_config, get_endpoint_by_name —
 *   백엔드가 endpoint 상태를 조회.
 * - spdk_vfu_map_one / unmap_sg: 게스트 PA → host VA 매핑 (libvfio-user
 *   의 vfu_addr_to_sgl + vfu_sgl_get) — bdev I/O 데이터 버퍼 준비에 필수.
 * - struct spdk_vfu_endpoint_ops: 11개 콜백 vtable — 백엔드 모듈이 구현.
 *   init/get_device_info/get_vendor_capability/attach_device/detach_device/
 *   destruct/post_memory_add/pre_memory_remove/reset_device/quiesce_device.
 * - struct spdk_vfu_pci_device: 백엔드가 채우는 PCI 디바이스 정보 — vendor/
 *   device ID, class code, PM/PCIe/MSI-X capability, region 들.
 * - struct spdk_vfu_pci_region: 단일 BAR 정의 — offset/len/flags/sparse mmap
 *   영역들 + access 콜백.
 */

#ifndef _VFU_TARGET_H
#define _VFU_TARGET_H

#include <vfio-user/libvfio-user.h>
/* [한국어] extern/libvfio-user 서브모듈의 공개 헤더. vfu_ctx_t (libvfio-user
 * 가 만드는 endpoint 컨텍스트 핸들), dma_sg_t (DMA scatter-gather 엔트리),
 * vfu_pci_type_t (PCI/PCIe 타입), VFU_PCI_DEV_NUM_REGIONS (region 배열 크기
 * 매크로) 등을 가져온다. SPDK 빌드 시 ./configure --with-vfio-user 가 켜져 있어야
 * 이 헤더가 빌드 트리에 노출된다. */
#include <vfio-user/pci_defs.h>
/* [한국어] PCI/PCIe capability 구조체 정의 (struct pmcap = Power Management
 * Capability, struct pxcap = PCI Express Capability, struct msixcap = MSI-X
 * Capability). 모두 PCI 로컬 버스 스펙 / PCIe Base Spec 의 capability 헤더
 * 비트필드를 그대로 본뜬 packed 구조체로, 가상 PCI config space 를 채울 때 사용. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러로 인클루드되어도 C 링크리지를 보장 — SPDK 라이브러리
 * (C ABI) 와 링크 호환을 유지하기 위함. */
#endif

/**
 * Callback for spdk_vfu_init().
 *
 * \param rc 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_vfu_init_cb - vfio-user 타깃 환경 초기화 완료 콜백 타입
 *
 * @rc: 0=초기화 성공, 음수 errno=실패 (libvfio-user 글로벌 init 실패,
 *      poller 등록 실패 등).
 *
 * spdk_vfu_init() 가 비동기로 동작할 수 있게 하는 SPDK 의 표준 패턴 — 호출자는
 * 이 콜백을 통해 초기화 완료 시점에 후속 작업(예: RPC 서버 활성화)을 시작한다.
 * 콜백은 spdk_vfu_init() 를 호출한 thread 와 동일한 spdk_thread 컨텍스트에서
 * 실행된다 (lib/vfu_tgt 가 send_msg 로 보장).
 */
typedef void (*spdk_vfu_init_cb)(int rc);


/**
 * Callback for spdk_vfu_fini().
 */
/*
 * [한국어]
 * spdk_vfu_fini_cb - vfio-user 타깃 환경 정리 완료 콜백 타입
 *
 * 인자/반환 없음. spdk_vfu_fini() 가 모든 endpoint 폐기와 libvfio-user 글로벌
 * 종료를 마치면 한 번 호출된다. SPDK 앱 종료(spdk_app_stop) 경로에서 사용.
 */
typedef void (*spdk_vfu_fini_cb)(void);

/**
 * Initialize vfio-user target environment.
 */
/*
 * [한국어]
 * spdk_vfu_init - vfio-user 타깃 서브시스템 글로벌 초기화
 *
 * @init_cb: 초기화 완료 시 호출될 콜백. NULL 불가 (보통 RPC subsystem init
 *           완료 시그널을 위해 항상 지정).
 *
 * 이 함수가 필요한 이유: libvfio-user 라이브러리는 1회만 글로벌 초기화되어야
 * 하며, 또한 SPDK 측에서는 endpoint 들의 lifecycle 을 추적하는 전역 리스트와
 * 기본 socket basename 을 셋업해야 한다. 이 함수는 다음을 수행한다:
 *   1) 내부 endpoint 리스트(TAILQ) 초기화.
 *   2) 기본 socket basename 을 SPDK runtime dir(/var/tmp 등)로 설정.
 *   3) endpoint 콜백 디스패치 작업을 위한 스레드 컨텍스트 준비.
 *   4) 비동기로 init_cb(0) 호출.
 *
 * 실행 컨텍스트: SPDK subsystem init 단계 (app 시작 시점). 단일 스레드.
 *
 * 호출 체인:
 *   spdk_subsystem_init → vfu_tgt_subsystem_init → [본 함수] → init_cb.
 */
void spdk_vfu_init(spdk_vfu_init_cb init_cb);

/**
 * Clean up vfio-user target environment.
 */
/*
 * [한국어]
 * spdk_vfu_fini - vfio-user 타깃 서브시스템 글로벌 정리
 *
 * @fini_cb: 모든 정리 완료 시 호출될 콜백.
 *
 * 동작: 등록된 모든 endpoint 에 대해 spdk_vfu_delete_endpoint() 를 호출 →
 *       libvfio-user 컨텍스트 destroy → 소켓 파일 unlink → endpoint 리스트
 *       비우기 → fini_cb() 호출. 외부 client 가 연결되어 있다면 해당
 *       세션은 강제 종료된다.
 *
 * 실행 컨텍스트: SPDK subsystem fini 단계 (app 종료 시점).
 *
 * 호출 체인:
 *   spdk_subsystem_fini → vfu_tgt_subsystem_fini → [본 함수] → fini_cb.
 */
void spdk_vfu_fini(spdk_vfu_fini_cb fini_cb);

/**
 * Opaque handle to a PCI endpoint, it's representative of a Unix Domain socket file.
 */
struct spdk_vfu_endpoint;
/* [한국어] vfio-user 타깃 endpoint 의 불투명 핸들(forward declaration). 1개의
 * endpoint 는 1개의 Unix 도메인 소켓 파일에 대응하며, 그 소켓에 client 1명이
 * 연결되면 가상 PCI 디바이스 1대가 활성화된다. 실제 정의는 lib/vfu_tgt/
 * tgt_endpoint.c 내부에 있고 다음 정도의 필드를 가진다(요지):
 *   - vfu_ctx_t *vfu_ctx (libvfio-user 컨텍스트)
 *   - struct spdk_vfu_endpoint_ops *ops (백엔드 vtable)
 *   - void *backend_ctx (백엔드 private 데이터)
 *   - struct spdk_vfu_pci_device pci (위 vtable 의 get_device_info 결과 캐시)
 *   - char name / socket path
 *   - struct spdk_thread *thread (이 endpoint 를 처리하는 SPDK 스레드)
 *   - poller, MSI-X 상태, attach 여부 등
 * 외부에는 불투명 포인터로만 노출되어 백엔드/RPC 가 캡슐화 위반 없이 다룰 수 있다. */

#define SPDK_VFU_MAX_NAME_LEN (64)
/* [한국어] endpoint 이름 / 백엔드 device type 이름의 최대 길이 (NUL 포함).
 * struct spdk_vfu_endpoint_ops::name 배열 크기 등에 사용. 64 바이트는 short
 * 식별자로 충분 — RPC 로 자유 입력되는 사용자 문자열 길이를 제한하기도 한다. */

/**
 * Vfio-user PCI device sparse MMAP region.
 *
 * The sparse mmap allows finer granularity of specifying areas
 * within a PCI region with mmap support.
 */
/*
 * [한국어]
 * struct spdk_vfu_sparse_mmap - PCI region 내부 sparse mmap 영역 1개
 *
 * 단일 PCI region(BAR) 안에서 client 가 직접 mmap 할 수 있는 sub-area 를
 * 표현한다. 예를 들어 NVMe BAR0 의 경우 컨트롤러 레지스터(0x0~0x1000) 는
 * 메시지 기반 access 콜백으로 처리하고 그 뒤의 도어벨 영역은 mmap 으로
 * client 가 zero-copy 로 직접 쓰게 만들 수 있다. 이를 위해 BAR 전체가 아닌
 * 부분 영역을 sparse 하게 지정한다.
 */
struct spdk_vfu_sparse_mmap {
	/**
	 * Sparse mmap region offset, starts from 0 of current PCI region.
	 */
	uint64_t offset;
	/* [한국어] 현재 PCI region(BAR) 시작점 기준 오프셋(바이트). 페이지(보통
	 * 4 KiB) 정렬되어야 함 — mmap() 의 정렬 요구. NVMe 도어벨이라면 보통
	 * 0x1000 (CAP.DSTRD 따라 다를 수 있음). */

	/**
	 * Sparse mmap region length.
	 */
	uint64_t len;
	/* [한국어] mmap 영역의 바이트 길이. 페이지 정렬되어야 함. client 는
	 * VFIO_USER_DEVICE_GET_REGION_IO_FDS 응답으로 이 (offset, len) 정보와
	 * fd 를 받아 mmap 한다. */
};

#define SPDK_VFU_MAXIMUM_SPARSE_MMAP_REGIONS	8
/* [한국어] 한 PCI region 당 sparse mmap 영역의 최대 개수. 8 은 NVMe/virtio
 * 디바이스가 실제로 사용하는 수보다 충분히 큰 값으로 채택. 배열 정적 할당
 * (struct spdk_vfu_pci_region::mmaps[]) 에 사용. */

/**
 * Callback for vfio-user PCI region access.
 *
 * \param vfu_ctx Opaque value of the PCI endpoint, it's created by libvfio-user.
 * \param buf data buffer to R/W.
 * \param count R/W size, the value could be 1,2,4,8.
 * \param pos offset from PCI region.
 * \param is_write true if access is WRITE.
 *
 * \return count on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_vfu_access_cb - PCI region(BAR) read/write 시 백엔드 모듈에 위임되는 콜백
 *
 * @vfu_ctx:  libvfio-user 가 만든 endpoint 컨텍스트 — vfu_get_private(vfu_ctx)
 *            로 백엔드 자기 데이터에 접근 가능.
 * @buf:      R/W 데이터 버퍼. is_write=true 면 client 가 보낸 값(읽기만), false 면
 *            백엔드가 채워서 client 에게 응답으로 돌려줌(쓰기만).
 * @count:    R/W 바이트 수 — 1/2/4/8 (PCI MMIO 의 정렬된 워드 사이즈).
 * @pos:      해당 PCI region 시작 기준 오프셋. NVMe 라면 컨트롤러 레지스터
 *            오프셋(CC=0x14, CSTS=0x1C, AQA=0x24, ASQ=0x28, ACQ=0x30, …) 또는
 *            도어벨 오프셋.
 * @is_write: true=client→server 쓰기 요청, false=client←server 읽기 요청.
 * @return:   성공 시 처리된 바이트 수 (보통 count), 실패 시 음수 errno (-EINVAL
 *            등). libvfio-user 가 그 값을 그대로 vfio_user_header.error_no
 *            로 client 에 전달.
 *
 * 이 함수가 필요한 이유: 외부 client 가 보낸 BAR R/W 메시지를 백엔드 모듈
 * (가상 NVMe 컨트롤러, 가상 virtio device 등)이 어떻게 처리할지를 위임하는
 * vtable 의 핵심. 백엔드는 이 콜백 안에서 가상 레지스터 상태 머신을 굴린다 —
 * 예: CC.EN=1 이 쓰여지면 admin 큐 초기화, AQA/ASQ/ACQ 가 쓰여지면 admin 큐
 * 자료구조 셋업, doorbell 이 쓰여지면 SQ tail 갱신 → bdev_io 발사.
 *
 * 실행 컨텍스트: endpoint 에 핀된 spdk_thread 위. libvfio-user 의 메시지
 * 디스패치에서 동기적으로 호출되므로 non-blocking 이어야 하며, 긴 I/O 는
 * bdev 비동기 제출 후 완료 콜백으로 처리해야 한다.
 */
typedef ssize_t (*spdk_vfu_access_cb)(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t pos,
				      bool is_write);

/**
 * Vfio-user device PCI region.
 *
 * PCI region is definition of PCI device BAR.
 */
/*
 * [한국어]
 * struct spdk_vfu_pci_region - 가상 PCI 디바이스의 단일 region(BAR) 정의
 *
 * VFU_PCI_DEV_NUM_REGIONS(=10 정도, libvfio-user 정의) 개의 region 슬롯이
 * struct spdk_vfu_pci_device::regions[] 배열로 존재하며, 백엔드 모듈은
 * 자신이 사용하는 BAR(보통 BAR0, MSI-X 의 경우 BAR4 등)에만 이 구조체를
 * 채운다. 사용하지 않는 region 은 len=0 으로 두어 비활성화.
 *
 * 한 region 은 두 가지 접근 모드를 동시에 지원할 수 있다:
 *   1) 메시지 기반 R/W: access_cb 콜백으로 client 의 BAR access 메시지를 처리.
 *   2) sparse mmap: mmaps[] 의 (offset, len) 영역들은 client 가 fd 를 mmap 해서
 *      직접 읽고 쓸 수 있게 노출 (zero-copy hot path — NVMe 도어벨용).
 */
struct spdk_vfu_pci_region {
	/**
	 * Offset of the PCI region.
	 */
	uint64_t offset;
	/* [한국어] 이 region 의 시작 오프셋(BAR 자체는 PCI config space 의 BAR
	 * 레지스터로 베이스가 정해지므로 보통 0). 일반적으로 0 으로 두고 BAR
	 * 단위로 시작점을 가정한다. */

	/**
	 * Length of the PCI region.
	 */
	uint64_t len;
	/* [한국어] region 의 바이트 길이. 0 이면 이 region 슬롯 비활성. NVMe BAR0
	 * 는 보통 0x1000 + (도어벨 영역) ≈ 16 KiB ~ 256 KiB. */

	/**
	 * Capability flags.
	 */
	uint64_t flags;
	/* [한국어] libvfio-user 에 전달되는 region capability 비트(예:
	 * VFU_REGION_FLAG_RW, FLAG_MEM, FLAG_MMAP). 어떤 access 가 허용되는지와
	 * mmap 가능 여부를 결정. */

	/**
	 * Number of sparse mmap region.
	 */
	uint32_t nr_sparse_mmaps;
	/* [한국어] 아래 mmaps[] 배열에서 유효한 엔트리 수 (0 ~ MAX). 0 이면 sparse
	 * mmap 을 제공하지 않고 모든 access 를 access_cb 로 처리. */

	/**
	 * Representative of the PCI region access file descriptor.
	 */
	int fd;
	/* [한국어] sparse mmap 영역들이 매핑될 backing fd. 보통 백엔드가
	 * memfd_create() 또는 shm_open() 으로 만든 익명 메모리 fd 를 둔다. client
	 * 는 이 fd 를 SCM_RIGHTS 로 받아 mmap. -1 이면 sparse mmap 비활성. */

	/**
	 * Sparse mmap regions.
	 */
	struct spdk_vfu_sparse_mmap mmaps[SPDK_VFU_MAXIMUM_SPARSE_MMAP_REGIONS];
	/* [한국어] sparse mmap (offset, len) 엔트리 배열. 위 nr_sparse_mmaps 만큼
	 * 유효. 페이지 정렬된 sub-region 들로, 같은 region 안에서 mmap 가능 영역
	 * 과 메시지 R/W 영역을 혼합 가능. */

	/**
	 * PCI region access callback.
	 */
	spdk_vfu_access_cb access_cb;
	/* [한국어] 메시지 기반 R/W 콜백. sparse mmap 영역 밖의 access 또는 sparse
	 * mmap 비활성 region 의 모든 access 가 이 콜백으로 들어온다. NULL 이면
	 * 해당 영역 access 시 client 에 -EINVAL 응답. */
};

/**
 * Vfio-user PCI device information.
 *
 * vfio-user target uses this data structure to get all the information
 * from backend emulated device module.
 */
/*
 * [한국어]
 * struct spdk_vfu_pci_device - 가상 PCI 디바이스의 종합 메타데이터
 *
 * vtable 의 get_device_info() 콜백이 채우는 출력 구조체. lib/vfu_tgt 가 이
 * 정보를 받아 libvfio-user 에 디바이스 셋업(vfu_pci_init, vfu_setup_region,
 * vfu_pci_add_capability 등)을 지시한다. PCI config space (256 바이트) +
 * PCIe extended config (4096 바이트) 의 표준 capability 들이 여기에 모두
 * 표현된다.
 */
struct spdk_vfu_pci_device {
	struct {
		/* Vendor ID */
		uint16_t vid;
		/* [한국어] PCI Vendor ID — config space offset 0x00. NVMe-vfio 의 경우
		 * 보통 SPDK 가 정의한 가짜 vendor ID 또는 실제 SSD vendor ID 흉내. */

		/* Device ID */
		uint16_t did;
		/* [한국어] PCI Device ID — config offset 0x02. virtio-blk/scsi 는
		 * VIRTIO 스펙 정의값(0x1042/0x1048 등) 사용. */

		/* Subsystem Vendor ID */
		uint16_t ssvid;
		/* [한국어] Subsystem Vendor ID — config offset 0x2C. 보통 vid 와 동일하게 둠. */

		/* Subsystem ID */
		uint16_t ssid;
		/* [한국어] Subsystem Device ID — config offset 0x2E. */
	} id;
	/* [한국어] PCI 식별자 4개 묶음. 호스트(QEMU) PCI bus 에 노출될 때 게스트
	 * OS 의 lspci 결과 vendor:device 표시에 그대로 반영된다. */

	struct {
		/* Base Class Code */
		uint8_t bcc;
		/* [한국어] Base Class Code — config offset 0x0B. NVMe 는 0x01(Mass
		 * Storage), virtio 는 다양. */

		/* Sub Class code */
		uint8_t scc;
		/* [한국어] Sub Class Code — config offset 0x0A. NVMe 는 0x08(NVM). */

		/* Programming Interface */
		uint8_t pi;
		/* [한국어] Programming Interface — config offset 0x09. NVMe 는 0x02
		 * (NVM Express). 게스트 OS 의 NVMe 드라이버 바인딩 키. */
	} class;
	/* [한국어] PCI Class Code 3개 묶음 (24 비트). 게스트 OS 가 어떤 드라이버를
	 * 바인딩할지 결정하는 핵심 정보. */

	/* Standard Power Management capabilities */
	struct pmcap pmcap;
	/* [한국어] PCI Power Management Capability (cap ID 0x01) — D0/D3hot 전이
	 * 등. 가상 디바이스라도 게스트 OS 가 PM 을 기대하므로 표준 형태로 채워둠.
	 * struct pmcap 정의는 <vfio-user/pci_defs.h> 의 packed 비트필드. */

	/* Standard PCI Express Capability ID */
	struct pxcap pxcap;
	/* [한국어] PCI Express Capability (cap ID 0x10) — PCIe link/device 정보,
	 * device control 비트 등. NVMe 는 PCIe 디바이스이므로 필수. */

	/* Standard MSI-X Capability */
	struct msixcap msixcap;
	/* [한국어] MSI-X Capability (cap ID 0x11) — MSI-X 테이블 BIR(BAR index)/
	 * offset/size, PBA(Pending Bit Array) 위치, table size. NVMe 는 큐별
	 * 인터럽트를 위해 MSI-X 가 사실상 필수. */

	/* Number of vendor specific capabilities */
	uint16_t nr_vendor_caps;
	/* [한국어] vtable 의 get_vendor_capability() 가 제공할 vendor-specific
	 * capability(cap ID 0x09) 의 개수. SPDK 가 그 수만큼 콜백을 호출해 각각
	 * 의 데이터를 가져온다. virtio-vfio 의 PCI capability 들이 이 경로로 나옴. */

	/* Legacy interrupt pin number */
	uint16_t intr_ipin;
	/* [한국어] PCI INTx 핀 번호 (1=INTA, 2=INTB, 3=INTC, 4=INTD, 0=비활성).
	 * config space offset 0x3D 에 들어감. MSI-X 만 쓸 거라면 0/1 로 두어도 무방. */

	/* Number of legacy interrupts */
	uint32_t nr_int_irqs;
	/* [한국어] INTx 인터럽트 수. 보통 1 (PCI 함수당 단일 INTx). */

	/* Number of MSIX interrupts */
	uint32_t nr_msix_irqs;
	/* [한국어] MSI-X 벡터 수. NVMe 는 admin 큐 1 + I/O 큐 N 만큼 (예: 32, 64,
	 * 128). msixcap.mxc.ts (table size - 1) 와 일관되게 채워야 함. */

	/* PCI regions */
	struct spdk_vfu_pci_region regions[VFU_PCI_DEV_NUM_REGIONS];
	/* [한국어] BAR0~BAR5 + ROM + PCI config space + 추가 region 들의 메타데이터.
	 * VFU_PCI_DEV_NUM_REGIONS 는 libvfio-user 가 정의 (보통 10). 사용 안 하는
	 * region 은 len=0. */
};

/*
 * [한국어]
 * struct spdk_vfu_endpoint_ops - 백엔드 PCI 디바이스 모듈의 vtable
 *
 * 백엔드 모듈(예: vfu_virtio_blk, vfu_virtio_scsi, NVMe-vfio)은 자신의
 * 동작을 11개의 콜백으로 구현해 spdk_vfu_register_endpoint_ops() 로 등록한다.
 * 이후 lib/vfu_tgt 가 endpoint lifecycle 의 각 단계에서 적절한 콜백을 호출.
 *
 * 호출 패턴 (한 endpoint 의 lifetime):
 *   create_endpoint(name, type) → ops 검색
 *     → init() 호출, 백엔드가 자기 데이터 할당 + spdk_vfu_pci_device 채움 준비
 *     → get_device_info() 호출, PCI 메타데이터 수집
 *     → libvfio-user 가 socket listen 시작 → client 연결 대기
 *
 *   client connect 시:
 *     → attach_device() 호출, 백엔드 I/O 가능 상태로 전환
 *     → client 의 BAR R/W → spdk_vfu_pci_region::access_cb 직접 호출
 *     → client 의 DMA_MAP → post_memory_add() 호출
 *     → client 의 DMA_UNMAP → pre_memory_remove() 호출
 *     → client 의 DEVICE_RESET → reset_device(), quiesce_device()
 *
 *   client disconnect 또는 delete_endpoint() 시:
 *     → detach_device() → destruct() → 자원 해제.
 */
struct spdk_vfu_endpoint_ops {
	/**
	 * Backend emulated PCI device type name.
	 */
	char name[SPDK_VFU_MAX_NAME_LEN];
	/* [한국어] 백엔드 디바이스 타입 식별 문자열 (예: "virtio_blk",
	 * "virtio_scsi", "nvme"). spdk_vfu_create_endpoint(... dev_type_name)
	 * 의 인자와 일치하는 ops 가 선택된다. 등록 시 중복 검사 키. */

	/**
	 * Initialize endpoint to PCI device with base path.
	 */
	void *(*init)(struct spdk_vfu_endpoint *endpoint,
		      char *basename, const char *endpoint_name);
	/* [한국어] endpoint 생성 직후 호출. 백엔드 자기 자료구조를 할당하고
	 * 그 포인터를 반환 — lib/vfu_tgt 는 이를 endpoint 의 backend_ctx 로 저장
	 * (이후 spdk_vfu_get_endpoint_private() 가 같은 값을 반환).
	 * @basename: 소켓 base 디렉토리(예: /var/tmp). 백엔드가 추가 파일을 만들 때 활용.
	 * @endpoint_name: 사람이 읽는 endpoint 이름.
	 * 반환 NULL = 실패 → 생성 abort. */

	/**
	 * Get PCI device information from backend device module.
	 */
	int (*get_device_info)(struct spdk_vfu_endpoint *endpoint,
			       struct spdk_vfu_pci_device *device_info);
	/* [한국어] 백엔드가 가상 PCI 디바이스의 메타데이터(VID/DID/class/cap/region)
	 * 를 device_info 에 채워 반환. lib/vfu_tgt 가 이걸 받아 libvfio-user 에
	 * vfu_pci_init/vfu_setup_region/vfu_pci_add_capability 호출.
	 * 반환 0 성공, 음수 errno 실패. */

	/**
	 * Get vendor capability based on ID in PCI configuration space.
	 */
	uint16_t (*get_vendor_capability)(struct spdk_vfu_endpoint *endpoint, char *buf,
					  uint16_t buf_len, uint16_t idx);
	/* [한국어] vendor-specific PCI capability(cap ID 0x09) 1개 분량을 buf 에
	 * 채워 길이를 반환. spdk_vfu_pci_device::nr_vendor_caps 만큼 idx=0,1,...
	 * 로 반복 호출됨. virtio 디바이스의 VIRTIO PCI capability 체인이 이 경로.
	 * 반환 0 = 더 이상 capability 없음. */

	/**
	 * Attach active connection to the PCI endpoint.
	 */
	int (*attach_device)(struct spdk_vfu_endpoint *endpoint);
	/* [한국어] client 가 socket 에 연결되었을 때 호출. 백엔드가 가상 디바이스
	 * 를 활성화 (예: virtio queue 풀, NVMe 컨트롤러 enable 준비). 보통 bdev
	 * open + I/O channel 할당이 일어남. 음수 errno 반환 시 연결 거부. */

	/**
	 * Detach the active connection of the PCI endpoint.
	 */
	int (*detach_device)(struct spdk_vfu_endpoint *endpoint);
	/* [한국어] client 가 disconnect 했을 때 호출. 진행 중 I/O 정리, 가상
	 * 디바이스 reset, bdev close 등. attach_device 의 역. */

	/**
	 * Destruct the PCI endpoint.
	 */
	int (*destruct)(struct spdk_vfu_endpoint *endpoint);
	/* [한국어] endpoint 삭제(spdk_vfu_delete_endpoint) 시 호출. init() 에서
	 * 할당한 backend_ctx 를 해제. 호출 후 endpoint 자체가 free 되므로 백엔드
	 * 자료에 더 이상 접근하면 안 된다. */

	/**
	 * Post-notification to backend module after a new memory region is added.
	 */
	int (*post_memory_add)(struct spdk_vfu_endpoint *endpoint, void *map_start, void *map_end);
	/* [한국어] client 가 VFIO_USER_DMA_MAP 으로 메모리 영역을 등록한 후 백엔드
	 * 에 통보. 백엔드는 spdk_mem_register() 등으로 그 영역을 SPDK 메모리
	 * 도메인에 추가해 NVMe DMA 등이 가능하게 만들 수 있다.
	 * @map_start/map_end: 호스트 가상주소 범위 (libvfio-user 가 mmap 한 결과). */

	/**
	 * Pre-notification to backend module before removing the memory region.
	 */
	int (*pre_memory_remove)(struct spdk_vfu_endpoint *endpoint, void *map_start, void *map_end);
	/* [한국어] client 가 VFIO_USER_DMA_UNMAP 을 보내기 직전 백엔드에 통보. 백엔드
	 * 는 진행 중 I/O 가 이 영역을 사용 중이면 완료를 기다리고, spdk_mem_unregister()
	 * 로 등록 해제. 콜백 반환 후 libvfio-user 가 munmap 수행. */

	/**
	 * PCI device reset callback.
	 */
	int (*reset_device)(struct spdk_vfu_endpoint *endpoint);
	/* [한국어] client 가 VFIO_USER_DEVICE_RESET 명령을 보냈거나, 가상 PCI
	 * 의 FLR(Function Level Reset) / NVMe CC.EN 0 전이 등이 발생했을 때 호출.
	 * 모든 큐/상태를 초기화. */

	/**
	 * PCI device quiesce callback, after this callback, the backend device module
	 * should stopping processing any IOs.
	 */
	int (*quiesce_device)(struct spdk_vfu_endpoint *endpoint);
	/* [한국어] reset 또는 라이브 마이그레이션 등 임계 작업 직전 I/O 정지를
	 * 요청. 콜백 반환 시점부터 백엔드는 새 I/O 를 받지 않고, 진행 중 I/O 도
	 * 완료까지 대기. SPDK_POLLER_BUSY 가 될 수 있어 비동기로 quiesce 완료를
	 * 알리는 변형 인터페이스가 lib/vfu_tgt 내부에 추가로 존재. */
};

/**
 * Register the operations of emulated backend PCI device.
 *
 * \param ops The operations of emulated backend PCI device.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_vfu_register_endpoint_ops - 백엔드 vtable 등록
 *
 * @ops: 백엔드가 정적으로 정의한 vtable 포인터. ops->name 이 식별 키.
 * @return: 0=성공, -EEXIST=같은 이름 이미 등록, -ENOMEM=내부 리스트 할당 실패.
 *
 * 이 함수가 필요한 이유: lib/vfu_tgt 는 백엔드를 컴파일타임에 알 필요가 없고,
 * 백엔드 모듈이 SPDK 모듈 init 에서 자기 vtable 을 등록하면 lib/vfu_tgt 가
 * dev_type_name 으로 lookup 한다. 일반적인 SPDK 모듈 등록 패턴
 * (spdk_module_register 와 유사).
 *
 * 호출 체인:
 *   백엔드 모듈 init (예: vfu_virtio_blk_module_init) → [본 함수].
 */
int spdk_vfu_register_endpoint_ops(struct spdk_vfu_endpoint_ops *ops);

/**
 * Create a PCI endpoint.
 *
 * \param endpoint_name Name of the PCI endpoint.
 * \param cpumask_str CPU masks that the endpoint is running on.
 * \param dev_type_name Name of the registered operation.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_vfu_create_endpoint - vfio-user endpoint(=Unix 소켓) 생성
 *
 * @endpoint_name: endpoint 식별 이름. 소켓 파일 이름의 일부로 사용 — 최종
 *                 경로는 "<basename>/<endpoint_name>".
 * @cpumask_str:   이 endpoint 의 콜백을 실행할 CPU 마스크 문자열 (예: "0x1",
 *                 "[0,2]"). spdk_thread 가 이 코어 중 하나에 핀된다.
 * @dev_type_name: 미리 register_endpoint_ops 로 등록된 백엔드 vtable 이름.
 *                 일치하는 ops 가 없으면 -ENODEV.
 * @return: 0=성공, 음수 errno=실패.
 *
 * 이 함수가 필요한 이유: 외부 client(QEMU 등)가 연결할 수 있는 가상 PCI
 * 디바이스 인스턴스를 동적으로 생성. 단계:
 *   1) ops lookup, 중복 endpoint_name 검사.
 *   2) endpoint 자료 할당 → ops->init() 호출 → backend_ctx 저장.
 *   3) ops->get_device_info() 호출, PCI 메타 수집.
 *   4) libvfio-user vfu_create_ctx + vfu_pci_init + vfu_setup_region 등 호출.
 *   5) socket bind + listen + SPDK poller 등록 → client 연결 대기.
 *
 * 실행 컨텍스트: RPC 핸들러 또는 앱 init 스레드. 비교적 무거운 동기 작업
 * (소켓 bind 등) 을 포함하므로 hot path 에서 호출되지 않음.
 *
 * 호출 체인:
 *   RPC: rpc_vfu_create_endpoint → [본 함수].
 */
int spdk_vfu_create_endpoint(const char *endpoint_name, const char *cpumask_str,
			     const char *dev_type_name);

/**
 * Delete a PCI endpoint.
 *
 * \param endpoint_name Name of the PCI endpoint.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_vfu_delete_endpoint - endpoint 삭제 (소켓 파일 + 자원 정리)
 *
 * @endpoint_name: 삭제할 endpoint 이름.
 * @return: 0=성공, -ENOENT=없음, 그 외 음수 errno=정리 실패.
 *
 * 동작: 1) endpoint 검색 → 2) client 연결 중이면 detach_device() 호출 →
 *       3) libvfio-user 컨텍스트 destroy + socket unlink → 4) destruct()
 *       호출로 백엔드 자료 해제 → 5) 전역 리스트에서 제거 + endpoint free.
 *
 * 호출 체인:
 *   RPC: rpc_vfu_delete_endpoint → [본 함수].
 *   또는 spdk_vfu_fini() 에서 모든 endpoint 에 대해 일괄 호출.
 */
int spdk_vfu_delete_endpoint(const char *endpoint_name);

/**
 * Set the base path to create socket files.
 *
 * \param basename Path to create socket files.
 *
 * \return 0 on success, negative errno on failure.
 */
/*
 * [한국어]
 * spdk_vfu_set_socket_path - 모든 endpoint 의 소켓 파일이 만들어질 기본 디렉토리 설정
 *
 * @basename: 절대 경로 (예: "/var/tmp"). 디렉토리가 존재해야 함.
 * @return: 0=성공, -EINVAL=경로 길이/형식 오류, -ENOENT=디렉토리 없음.
 *
 * 동작: 내부 전역 변수 g_socket_basename 을 갱신. 이후 create_endpoint 의
 *       소켓 경로가 "<basename>/<endpoint_name>" 으로 결정된다. 기존 endpoint
 *       에는 영향 없음 — 이미 listen 중인 socket 경로는 유지.
 *
 * 호출 체인:
 *   RPC: rpc_vfu_target_set_base_path → [본 함수].
 */
int spdk_vfu_set_socket_path(const char *basename);

/**
 * Get UUID of the PCI endpoint.
 *
 * This function will return the absolute path of the PCI endpoint which
 * represented by a socket file.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return absolute path of the PCI endpoint.
 */
/*
 * [한국어]
 * spdk_vfu_get_endpoint_id - endpoint 의 절대 socket 경로(=식별자) 반환
 *
 * @endpoint: 대상 endpoint 핸들.
 * @return:   "<basename>/<endpoint_name>" 형식의 const C 문자열. 백엔드가
 *            로깅이나 RPC 응답에 사용. NULL 반환 없음(유효 endpoint 가정).
 *
 * 이 경로가 곧 vfio-user "UUID" — client 는 이 경로로 connect() 한다. 게스트
 * QEMU 의 "-device vfio-user-pci,socket=<id>" 인자와 일치.
 */
const char *spdk_vfu_get_endpoint_id(struct spdk_vfu_endpoint *endpoint);

/**
 * Get name of the PCI endpoint.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return name of the PCI endpoint.
 */
/*
 * [한국어]
 * spdk_vfu_get_endpoint_name - endpoint 의 짧은 이름(create 시 인자) 반환
 *
 * @endpoint: 대상 endpoint.
 * @return:   create_endpoint 의 endpoint_name 인자 문자열. 로깅/RPC dump 용.
 */
const char *spdk_vfu_get_endpoint_name(struct spdk_vfu_endpoint *endpoint);

/**
 * Get opaque handle of the PCI endpoint that created by libvfio-user library.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return opaque handle on success, NULL on failure.
 */
/*
 * [한국어]
 * spdk_vfu_get_vfu_ctx - 내부 libvfio-user 컨텍스트(vfu_ctx_t*) 반환
 *
 * @endpoint: 대상 endpoint.
 * @return:   libvfio-user 가 만든 vfu_ctx_t 포인터. 실패 시 NULL.
 *
 * 이 함수가 필요한 이유: 백엔드 모듈이 libvfio-user 의 저수준 API
 * (vfu_irq_trigger, vfu_addr_to_sgl, vfu_create_ioeventfd 등) 를 직접 호출해야
 * 할 때 사용. 예: NVMe-vfio target 이 큐 완료 시 MSI-X 인터럽트를 발사하기 위해
 * vfu_irq_trigger(ctx, vector). spdk_vfu_map_one() 는 이 ctx 를 내부에서 사용.
 */
vfu_ctx_t *spdk_vfu_get_vfu_ctx(struct spdk_vfu_endpoint *endpoint);

/**
 * Get private opaque handle of backend PCI device module.
 *
 * This function is used in backend PCI device module to get the internal
 * private data structure saved in vfu_target library.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return opaque handle of backend device on success, NULL on failure.
 */
/*
 * [한국어]
 * spdk_vfu_get_endpoint_private - 백엔드가 init() 에서 반환한 자기 자료 포인터 회수
 *
 * @endpoint: 대상 endpoint.
 * @return:   ops->init() 가 반환했던 void * 포인터 — 백엔드 자기 캐스트 후 사용.
 *            init() 미호출 또는 NULL 반환된 경우 NULL.
 *
 * 백엔드 콜백들이 endpoint 별 상태를 꺼내는 표준 진입점. 모든 vtable 콜백의
 * 첫 줄이 보통 "ctx = spdk_vfu_get_endpoint_private(endpoint)" 형태.
 */
void *spdk_vfu_get_endpoint_private(struct spdk_vfu_endpoint *endpoint);

/**
 * MSI-X is enabled or not.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return true if MSI-X is enabled, false otherwise.
 */
/*
 * [한국어]
 * spdk_vfu_endpoint_msix_enabled - MSI-X 활성 여부 조회
 *
 * @endpoint: 대상 endpoint.
 * @return:   true=client 가 PCI config space 의 MSI-X Message Control 의
 *            Enable 비트(bit15)를 set 했음, false=비활성.
 *
 * 백엔드는 인터럽트 발사 직전 이 함수로 게스트가 MSI-X 를 켰는지 확인 — 꺼져
 * 있으면 INTx 폴백 또는 벡터 무시.
 */
bool spdk_vfu_endpoint_msix_enabled(struct spdk_vfu_endpoint *endpoint);

/**
 * INT-X is enabled or not.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return true if INT-X is enabled, false otherwise.
 */
/*
 * [한국어]
 * spdk_vfu_endpoint_intx_enabled - INTx legacy 인터럽트 활성 여부 조회
 *
 * @endpoint: 대상 endpoint.
 * @return:   true=PCI config space Command 레지스터(0x04) 의 INTx disable 비트
 *            (bit10) 가 0 이며 INTx 가 사용 가능, false=disable.
 *
 * 백엔드는 MSI-X 가 disable 인 경우 이 함수로 INTx 사용 가능성을 본다.
 */
bool spdk_vfu_endpoint_intx_enabled(struct spdk_vfu_endpoint *endpoint);

/**
 * Get PCI configuration space.
 *
 * \param endpoint The PCI endpoint.
 *
 * \return pointer to PCI configuration space on success, NULL on failure.
 */
/*
 * [한국어]
 * spdk_vfu_endpoint_get_pci_config - 가상 PCI config space (256B) 포인터 반환
 *
 * @endpoint: 대상 endpoint.
 * @return:   가상 PCI config space 메모리(라이브러리 내부 버퍼)의 시작 주소.
 *            BAR/Cap/Command 레지스터들이 이 256 바이트 영역에 맵된다. NULL =
 *            init 미완료.
 *
 * 백엔드가 표준 capability 외 사용자 정의 필드(예: vendor cap 안의 BAR 인덱스)
 * 를 직접 조작해야 할 때 사용. 일반적으로 직접 조작은 권장되지 않으며,
 * libvfio-user 의 vfu_pci_add_capability 등을 우선 사용한다.
 */
void *spdk_vfu_endpoint_get_pci_config(struct spdk_vfu_endpoint *endpoint);

/**
 * Get PCI endpoint via name.
 *
 * \param name The PCI endpoint name.
 *
 * \return PCI endpoint pointer on success, NULL on failure.
 */
/*
 * [한국어]
 * spdk_vfu_get_endpoint_by_name - 이름으로 endpoint 핸들 lookup
 *
 * @name:   create_endpoint 시 지정한 endpoint_name.
 * @return: 일치하는 endpoint 핸들, 없으면 NULL.
 *
 * RPC 핸들러나 백엔드 모듈이 외부 식별자로부터 endpoint 를 찾을 때 사용.
 * 내부 endpoint 리스트를 선형 탐색 (endpoint 수가 작아 OK).
 */
struct spdk_vfu_endpoint *spdk_vfu_get_endpoint_by_name(const char *name);

/**
 * Map Guest Physical Address to Host Virtual Address.
 *
 * \param endpoint The PCI endpoint.
 * \param addr Physical address that to be mapped.
 * \param len Length of mapped address.
 * \param sg Scatter/gather entry to be mapped.
 * \param iov IOV to save mapped virtual address and length.
 * \param prot Protection flags.
 *
 * \return mapped virtual address on success, NULL on failure.
 */
/*
 * [한국어]
 * spdk_vfu_map_one - 게스트 물리주소(IOVA)를 호스트 가상주소로 매핑 (1개 SG)
 *
 * @endpoint: 대상 endpoint.
 * @addr:     매핑할 게스트 물리(=IOVA) 시작 주소. client 가 사전에 DMA_MAP
 *            으로 등록한 영역 내부여야 함.
 * @len:      매핑 길이(바이트).
 * @sg:       호출자가 제공한 dma_sg_t 1개 슬롯 — libvfio-user 가 내부 추적용
 *            메타를 채워 넣는다. 후속 unmap_sg 에서 그대로 다시 사용.
 * @iov:      호출자가 제공한 struct iovec 1개 슬롯 — 매핑 결과의 호스트 VA 와
 *            length 가 채워져 반환.
 * @prot:     POSIX mmap prot 비트 (PROT_READ/PROT_WRITE). client 가 DMA_MAP
 *            때 부여한 권한과 교차 검증.
 * @return:   iov->iov_base 와 동일한 호스트 VA 포인터, 실패 시 NULL.
 *
 * 이 함수가 필요한 이유: client(VM)가 NVMe submission queue 에 PRP/SGL 로
 * 게스트 물리주소를 적어 두면, 백엔드가 그 주소를 host VA 로 변환해 bdev I/O
 * 의 데이터 버퍼 (struct iovec) 로 사용해야 한다. libvfio-user 의
 * vfu_addr_to_sgl + vfu_sgl_get 을 한 번에 묶은 thin wrapper.
 *
 * 단일 SG 만 매핑 가능하므로 PRP list 처럼 여러 페이지를 매핑하려면 페이지마다
 * 호출하거나 하위 libvfio-user API 직접 사용. 백엔드는 spdk_vfu_unmap_sg 로
 * 짝을 맞춰 해제해야 한다.
 *
 * 실행 컨텍스트: 백엔드의 I/O 처리 콜백 안 (BAR write → 도어벨 → I/O
 * 발사 경로). 동기 호출. host 메모리 페이지가 이미 매핑되어 있어 페이징/
 * page fault 가 발생하지 않는다.
 */
void *spdk_vfu_map_one(struct spdk_vfu_endpoint *endpoint, uint64_t addr, uint64_t len,
		       dma_sg_t *sg, struct iovec *iov, int prot);

/**
 * Unmap array of scatter/gather entries.
 *
 * \param endpoint The PCI endpoint.
 * \param array of scatter/gather entries to be unmapped.
 * \param iov array of IOVs contain virtual addresses and length.
 * \param iovcnt Number of IOVs.
 *
 */
/*
 * [한국어]
 * spdk_vfu_unmap_sg - spdk_vfu_map_one 으로 매핑한 영역들을 해제
 *
 * @endpoint: 대상 endpoint.
 * @sg:       map_one 으로 채워진 dma_sg_t 배열.
 * @iov:      대응하는 iovec 배열.
 * @iovcnt:   배열 원소 수 (sg 와 iov 모두 이 길이).
 *
 * 동작: libvfio-user 의 vfu_sgl_put 을 호출해 내부 매핑 카운트 감소. 마지막
 *       참조가 빠지면 page-pin 해제. 반환값 없음 — 실패해도 무시 (이중 unmap
 *       방지는 호출자 책임).
 *
 * 호출 시점: bdev I/O 완료 콜백 — completion 을 client 에 응답한 직후. 매핑
 * 이 살아 있는 동안에는 client 가 그 영역을 unmap 해도 백엔드의 unmap_sg
 * 가 끝나야 실제 release 되어 use-after-free 방지.
 */
void spdk_vfu_unmap_sg(struct spdk_vfu_endpoint *endpoint, dma_sg_t *sg, struct iovec *iov,
		       int iovcnt);
#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 종료. */
#endif

#endif
/* [한국어] _VFU_TARGET_H 헤더 가드 종료 — 다중 인클루드 시 재정의 방지. */
