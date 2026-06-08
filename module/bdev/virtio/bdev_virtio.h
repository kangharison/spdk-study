/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] Virtio bdev 모듈 공개 API 헤더 (bdev_virtio.h)
 *
 * === 파일의 역할 ===
 * Virtio-BLK / Virtio-SCSI 디바이스를 클라이언트(initiator) 측에서 사용하기 위한 bdev 모듈의
 * 공개 API. Virtio는 KVM/QEMU 등 가상화 환경에서 표준화된 PCI/MMIO 디바이스 인터페이스로,
 * "host"가 디바이스를 제공하고 "guest"가 사용한다. SPDK는 본 모듈로 다음 3가지 형태의 host와
 * 통신할 수 있는 initiator를 제공한다:
 *   1) vhost-user UNIX socket : SPDK vhost target과 동일 호스트에서 socket 기반 통신
 *   2) vfio-user UNIX socket : libvfio-user 기반 사용자 공간 통신
 *   3) virtio-pci : 실제 PCI 디바이스(emulated by QEMU 또는 SR-IOV VF)
 * 디바이스마다 SCSI 변종은 target/LUN scanning이 자동으로 일어나 다수의 bdev가 생성될 수 있고,
 * BLK 변종은 디바이스 1개당 bdev 1개로 매핑된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   [JSON-RPC] → [bdev_virtio_rpc.c]
 *     → [이 헤더]
 *         → bdev_virtio_scsi.c (Virtio-SCSI initiator: target scan, REPORT_LUNS, READ/WRITE)
 *         → bdev_virtio_blk.c (Virtio-BLK initiator)
 *             ↓ vhost-user/vfio-user/virtio-pci transport (lib/virtio, lib/vfio_user)
 *                 ↓ host에 있는 Virtio-SCSI/BLK 백엔드
 *
 * === 타 모듈과의 연결 ===
 * - lib/virtio/ : virtio queue 관리 (descriptor ring, avail/used).
 * - lib/vfio_user/ : vfio-user 프로토콜.
 * - include/spdk/env.h : PCI 주소 타입 spdk_pci_addr.
 *
 * === 주요 함수/구조체 요약 ===
 * - typedef bdev_virtio_create_cb / bdev_virtio_remove_cb : 비동기 콜백 시그니처.
 * - bdev_virtio_user_scsi_dev_create / bdev_vfio_user_scsi_dev_create /
 *   bdev_virtio_pci_scsi_dev_create : 3가지 Transport별 SCSI 디바이스 생성.
 * - bdev_virtio_user_blk_dev_create / bdev_virtio_vfio_user_blk_dev_create /
 *   bdev_virtio_pci_blk_dev_create : BLK 변종.
 * - bdev_virtio_scsi_dev_remove / bdev_virtio_blk_dev_remove : 삭제.
 * - bdev_virtio_pci_blk_set_hotplug : 핫플러그 polling 활성/주기 설정.
 */

#ifndef SPDK_BDEV_VIRTIO_H
#define SPDK_BDEV_VIRTIO_H

#include "spdk/bdev.h"
/* [한국어] bdev core 공개 API (spdk_bdev, unregister callback 등). */
#include "spdk/env.h"
/* [한국어] DPDK 환경 추상화. spdk_pci_addr 타입 사용. */

/**
 * Callback for creating virtio bdevs.
 *
 * \param ctx opaque context set by the user
 * \param errnum error code. 0 on success, negative errno on error.
 * \param bdevs contiguous array of created bdevs
 * \param bdev_cnt number of bdevs in the `bdevs` array
 */
/*
 * [한국어] typedef bdev_virtio_create_cb
 * SCSI 변종은 target scan 결과 N개의 bdev가 만들어질 수 있어 배열로 전달.
 * 호출 컨텍스트: scan 완료 시 — 호출자가 RPC를 호출했던 스레드에서 spdk_thread_send_msg로 복귀.
 */
typedef void (*bdev_virtio_create_cb)(void *ctx, int errnum,
				      struct spdk_bdev **bdevs, size_t bdev_cnt);

/**
 * Callback for removing virtio devices.
 *
 * \param ctx opaque context set by the user
 * \param errnum error code. 0 on success, negative errno on error.
 */
/*
 * [한국어] typedef bdev_virtio_remove_cb
 * Virtio 디바이스 제거 완료(또는 ENODEV/EBUSY) 콜백.
 */
typedef void (*bdev_virtio_remove_cb)(void *ctx, int errnum);

/**
 * Connect to a vhost-user Unix domain socket and create a Virtio SCSI device.
 * ...상세 영문 주석 유지...
 */
/*
 * [한국어] bdev_virtio_user_scsi_dev_create - vhost-user socket을 통해 Virtio-SCSI initiator 생성.
 *
 * @name: virtio 디바이스 이름. 자식 bdev들은 "<name>t<target_id>"로 명명됨.
 * @path: vhost-user UNIX socket 경로 (SPDK vhost target이 listen).
 * @num_queues: 사용할 request virtqueue 수 — 호스트가 적게 지원해도 OK.
 * @queue_size: 각 큐의 깊이.
 * @cb_fn / @cb_arg: target scan 완료 시 호출.
 * @return: 0 성공(스캔 시작), 음수 -errno.
 */
int bdev_virtio_user_scsi_dev_create(const char *name, const char *path,
				     unsigned num_queues, unsigned queue_size,
				     bdev_virtio_create_cb cb_fn, void *cb_arg);

/**
 * Connect to a vfio-user Unix domain socket and create a Virtio SCSI device.
 * ...상세 영문 주석 유지...
 */
/*
 * [한국어] bdev_vfio_user_scsi_dev_create - vfio-user socket 기반 SCSI initiator 생성.
 * vhost-user와 거의 동일하나 transport가 vfio-user 프로토콜 (PCI capability를 사용자 공간에서 모방).
 */
int bdev_vfio_user_scsi_dev_create(const char *base_name, const char *path,
				   bdev_virtio_create_cb cb_fn, void *cb_arg);

/**
 * Attach virtio-pci device. ...상세 영문 주석 유지...
 */
/*
 * [한국어] bdev_virtio_pci_scsi_dev_create - 실제 PCI 디바이스를 attach해 SCSI initiator 생성.
 *
 * @pci_addr: PCI BDF (Bus:Device:Function) 형태의 주소. DPDK가 pci 디바이스 enum 결과.
 */
int bdev_virtio_pci_scsi_dev_create(const char *name, struct spdk_pci_addr *pci_addr,
				    bdev_virtio_create_cb cb_fn, void *cb_arg);

/**
 * Remove a Virtio device with given name. ...상세 영문 주석 유지...
 */
/*
 * [한국어] bdev_virtio_scsi_dev_remove - SCSI virtio 디바이스 + 자식 bdev 모두 삭제.
 * @return: 0 성공, -ENODEV not found, -EBUSY 이미 삭제 진행 중.
 */
int bdev_virtio_scsi_dev_remove(const char *name,
				bdev_virtio_remove_cb cb_fn, void *cb_arg);

/**
 * Remove a Virtio device with given name. ...상세 영문 주석 유지...
 */
/*
 * [한국어] bdev_virtio_blk_dev_remove - BLK virtio 디바이스 삭제.
 * @return: 0/-ENODEV/-EINVAL(BLK 아님).
 */
int bdev_virtio_blk_dev_remove(const char *name,
			       bdev_virtio_remove_cb cb_fn, void *cb_arg);

/**
 * List all created Virtio-SCSI devices.
 *
 * \param write_ctx JSON context to write into
 */
/*
 * [한국어] bdev_virtio_scsi_dev_list - 등록된 모든 SCSI 디바이스를 JSON으로 출력.
 */
void bdev_virtio_scsi_dev_list(struct spdk_json_write_ctx *write_ctx);

/**
 * Connect to a vhost-user Unix domain socket and create a Virtio BLK bdev. ...
 */
/*
 * [한국어] bdev_virtio_user_blk_dev_create - vhost-user 기반 Virtio-BLK initiator 생성.
 * BLK는 single namespace이므로 bdev가 1개만 만들어진다 → 동기 반환.
 */
struct spdk_bdev *bdev_virtio_user_blk_dev_create(const char *name, const char *path,
		unsigned num_queues, unsigned queue_size);

/**
 * Connect to a vfio-user Unix domain socket and create a Virtio BLK bdev.
 */
/*
 * [한국어] bdev_virtio_vfio_user_blk_dev_create - vfio-user transport 기반 BLK.
 */
struct spdk_bdev *
bdev_virtio_vfio_user_blk_dev_create(const char *name, const char *path);

/**
 * Attach virtio-pci device. ...
 */
/*
 * [한국어] bdev_virtio_pci_blk_dev_create - PCI BLK 디바이스 attach.
 */
struct spdk_bdev *bdev_virtio_pci_blk_dev_create(const char *name,
		struct spdk_pci_addr *pci_addr);

/**
 * Enable/Disable the virtio blk hotplug monitor or change the monitor period time
 */
/*
 * [한국어] bdev_virtio_pci_blk_set_hotplug - PCI BLK 핫플러그 모니터 toggle.
 * @enabled: true면 polling poller 활성, false면 비활성.
 * @period_us: polling 주기(μs).
 */
int bdev_virtio_pci_blk_set_hotplug(bool enabled, uint64_t period_us);

#endif /* SPDK_BDEV_VIRTIO_H */
