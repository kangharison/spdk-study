/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] vfu_tgt(vfio-user 타깃) 엔드포인트 라이프사이클 구현 (tgt_endpoint.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK의 vfio-user 타깃 프레임워크(lib/vfu_tgt)의 코어 구현이다. vfio-user 프로토콜은
 * VFIO 인터페이스를 그대로 따라가면서 커널 대신 *유저스페이스* 프로세스가 가상 PCIe 디바이스를
 * 에뮬레이트할 수 있게 해주는 socket-based 프로토콜이다. SPDK는 libvfio-user 라이브러리를
 * 사용하여 가상 NVMe 컨트롤러나 SCSI 디바이스를 QEMU 같은 클라이언트에 노출시킨다.
 *
 * 본 파일이 담당하는 작업:
 *   (1) PCIe 디바이스 타입 ops 등록 — module/vfu_device/nvme/scsi가 init 시 자신의
 *       spdk_vfu_endpoint_ops를 등록한다.
 *   (2) endpoint(가상 PCIe 함수 한 개) 생성/소멸 — Unix 도메인 소켓 경로, cpumask, dev_type을
 *       받아 새 spdk_thread를 만들고 그 위에 accept_poller + vfu_ctx_poller를 등록.
 *   (3) libvfio-user 컨텍스트 realize — vfu_create_ctx → vfu_pci_init → vfu_pci_add_capability →
 *       vfu_setup_region(BAR들) → vfu_setup_device_dma_cb → vfu_realize_ctx.
 *   (4) DMA 메모리 region add/remove 콜백 — 클라이언트가 메모리 매핑을 알릴 때 spdk_mem_register로
 *       SPDK env에 등록하여 모든 SPDK 메모리 알림 콜백(예: rdma_utils_mem_notify)이 발화하게 함.
 *   (5) poller — accept_poller가 새 클라이언트 연결을 받고, vfu_ctx_poller가 매 100us마다
 *       vfu_run_ctx로 들어온 vfio-user 메시지를 처리.
 *   (6) endpoint별 spdk_thread를 생성/종료 — 각 endpoint는 자신의 thread에서 polling되어
 *       다른 endpoint와 격리.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   QEMU(클라이언트) ── Unix domain socket(stream) ──→ SPDK vfu_tgt accept_poller
 *     → vfu_attach_ctx로 새 연결 수락
 *     → endpoint->ops.attach_device(NVMe/SCSI 모듈 콜백)
 *     → 이후 매 polling cycle마다 vfu_run_ctx(endpoint->vfu_ctx)
 *       → libvfio-user가 PCI config / BAR access / DMA op 메시지를 디코드
 *       → 각각 endpoint->ops 콜백(예: nvmf admin/io cmd) 호출
 *   상위 caller: app/vfu_tgt + module/vfu_device/* (NVMe/SCSI)
 *   하위 callee: libvfio-user(vfu_create_ctx, vfu_run_ctx, vfu_pci_*, vfu_setup_*),
 *               SPDK thread (spdk_thread_create / spdk_poller_register),
 *               spdk_mem_register/unregister (DMA region 등록).
 *
 * === 타 모듈과의 연결 ===
 * - 외부 공개 API: include/spdk/vfu_target.h — spdk_vfu_init/fini/register_endpoint_ops/
 *   create_endpoint/delete_endpoint 등이 여기 선언됨.
 * - 등록 모듈: module/vfu_device/vfu_virtio_nvme.c, vfu_virtio_scsi.c 등이
 *   spdk_vfu_register_endpoint_ops로 자신의 콜백을 등록.
 * - RPC: lib/vfu_tgt/tgt_rpc.c가 vfu_tgt_set_base_path 메서드를 등록 → 본 파일의
 *   g_endpoint_path_dirname을 갱신.
 * - 글로벌 상태:
 *     g_tgt_core_mask          : 사용 가능 CPU 코어 비트맵.
 *     g_endpoint               : 활성 endpoint 리스트.
 *     g_pci_device_ops         : 등록된 PCI 디바이스 타입 ops 리스트.
 *     g_endpoint_path_dirname  : Unix socket base dir.
 *     g_fini_cb, g_fini_endpoint_cnt : spdk_vfu_fini 진행 추적.
 *   g_endpoint_lock으로 보호.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct tgt_pci_device_ops      : 등록된 디바이스 타입 ops + TAILQ 링크.
 * - spdk_vfu_register_endpoint_ops : 모듈이 자신의 콜백 묶음을 등록.
 * - spdk_vfu_set_socket_path       : Unix socket base dir 설정(RPC에서 호출).
 * - spdk_vfu_create_endpoint       : endpoint 객체 alloc + libvfio-user realize + 전용 thread 생성.
 * - spdk_vfu_delete_endpoint       : endpoint를 자신의 thread로 전송하여 정리.
 * - tgt_accept_poller              : libvfio-user 소켓에서 새 클라이언트 연결 수락.
 * - tgt_vfu_ctx_poller             : vfu_run_ctx로 들어온 PCI 메시지 처리(100us 주기).
 * - tgt_memory_region_add/remove_cb: 클라이언트 DMA 영역 add/remove → spdk_mem_register.
 * - tgt_endpoint_realize           : vfu_create_ctx + pci_init + caps + regions + realize.
 * - spdk_vfu_init / spdk_vfu_fini  : 프레임워크 초기화/종료.
 */

#include "spdk/stdinc.h"
/* [한국어] SPDK 표준 포함: stddef/stdint/string/unistd 등을 한 번에 가져옴. */
#include "spdk/env.h"
/* [한국어] SPDK env — SPDK_ENV_FOREACH_CORE 등. */
#include "spdk/thread.h"
/* [한국어] SPDK thread/poller API — spdk_thread_create, SPDK_POLLER_REGISTER 등. */
#include "spdk/log.h"
/* [한국어] 로그 매크로. */
#include "spdk/util.h"
/* [한국어] 유틸리티 — SPDK_CONTAINEROF 등. */
#include "spdk/memory.h"
/* [한국어] MASK_2MB 매크로 — 2MB 페이지 정렬 검사. */
#include "spdk/cpuset.h"
/* [한국어] spdk_cpuset — endpoint thread의 CPU affinity 표현. */
#include "spdk/likely.h"
/* [한국어] branch hint. */
#include "spdk/vfu_target.h"
/* [한국어] 외부 공개 API — spdk_vfu_endpoint_ops 등 타입 정의. */

#include "tgt_internal.h"
/* [한국어] 내부 헤더 — struct spdk_vfu_endpoint 정의. */

/*
 * [한국어]
 * struct tgt_pci_device_ops - 등록된 PCI 디바이스 타입 ops를 TAILQ로 묶는 wrapper
 *
 * spdk_vfu_endpoint_ops는 caller가 직접 등록하는 콜백 묶음이고, 이를 g_pci_device_ops 리스트에
 * 연결하기 위해 TAILQ 링크를 가진 wrapper 구조체로 감싼다.
 */
struct tgt_pci_device_ops {
	struct spdk_vfu_endpoint_ops ops;
	/* [한국어] 모듈(예: vfu_virtio_nvme)이 등록한 콜백 묶음 — name/init/destruct/attach/detach 등. */
	TAILQ_ENTRY(tgt_pci_device_ops) link;
	/* [한국어] g_pci_device_ops TAILQ 링크. 보호: g_endpoint_lock. */
};

static struct spdk_cpuset g_tgt_core_mask;
/* [한국어] 이 프로세스가 사용할 수 있는 CPU 비트맵 — SPDK_ENV_FOREACH_CORE로 채워짐.
 * endpoint cpumask는 이 마스크의 부분집합이어야 함. */
static pthread_mutex_t g_endpoint_lock = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] g_endpoint, g_pci_device_ops, g_fini_cb 모두 보호. RPC/init/fini 경로가 다른 스레드에서 호출될 수 있음. */
static TAILQ_HEAD(, spdk_vfu_endpoint) g_endpoint = TAILQ_HEAD_INITIALIZER(g_endpoint);
/* [한국어] 활성 endpoint 리스트 — 이름으로 검색해 중복 방지 및 fini 시 일괄 정리. */
static TAILQ_HEAD(, tgt_pci_device_ops) g_pci_device_ops = TAILQ_HEAD_INITIALIZER(g_pci_device_ops);
/* [한국어] 등록된 디바이스 타입 ops 리스트 — create_endpoint 시 dev_type 매치 검색. */
static char g_endpoint_path_dirname[PATH_MAX] = "";
/* [한국어] Unix socket base directory — endpoint UUID는 "<base>/<name>" 형태로 합성. */
static uint32_t g_fini_endpoint_cnt = 0;
/* [한국어] spdk_vfu_fini 진행 시 남은 endpoint 수 — 마지막 endpoint가 종료될 때 콜백 호출. */
static spdk_vfu_fini_cb g_fini_cb = NULL;
/* [한국어] fini 완료 콜백 — 모든 endpoint가 정리되면 호출. */

static struct spdk_vfu_endpoint_ops *
tgt_get_pci_device_ops(const char *device_type_name)
{
	struct tgt_pci_device_ops *pci_ops, *tmp;
	bool exist = false;

	pthread_mutex_lock(&g_endpoint_lock);
	TAILQ_FOREACH_SAFE(pci_ops, &g_pci_device_ops, link, tmp) {
		if (!strncmp(device_type_name, pci_ops->ops.name, SPDK_VFU_MAX_NAME_LEN)) {
			exist = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_endpoint_lock);

	if (exist) {
		return &pci_ops->ops;
	}
	return NULL;
}

int
spdk_vfu_register_endpoint_ops(struct spdk_vfu_endpoint_ops *ops)
{
	struct tgt_pci_device_ops *pci_ops;
	struct spdk_vfu_endpoint_ops *tmp;

	tmp = tgt_get_pci_device_ops(ops->name);
	if (tmp) {
		return -EEXIST;
	}

	pci_ops = calloc(1, sizeof(*pci_ops));
	if (!pci_ops) {
		return -ENOMEM;
	}
	pci_ops->ops = *ops;

	pthread_mutex_lock(&g_endpoint_lock);
	TAILQ_INSERT_TAIL(&g_pci_device_ops, pci_ops, link);
	pthread_mutex_unlock(&g_endpoint_lock);

	return 0;
}

static char *
tgt_get_base_path(void)
{
	return g_endpoint_path_dirname;
}

int
spdk_vfu_set_socket_path(const char *basename)
{
	int ret;

	if (basename && strlen(basename) > 0) {
		ret = snprintf(g_endpoint_path_dirname, sizeof(g_endpoint_path_dirname) - 2, "%s", basename);
		if (ret <= 0) {
			return -EINVAL;
		}
		if ((size_t)ret >= sizeof(g_endpoint_path_dirname) - 2) {
			SPDK_ERRLOG("Char dev dir path length %d is too long\n", ret);
			return -EINVAL;
		}

		if (g_endpoint_path_dirname[ret - 1] != '/') {
			g_endpoint_path_dirname[ret] = '/';
			g_endpoint_path_dirname[ret + 1]  = '\0';
		}
	}

	return 0;
}

struct spdk_vfu_endpoint *
spdk_vfu_get_endpoint_by_name(const char *name)
{
	struct spdk_vfu_endpoint *endpoint, *tmp;
	bool exist = false;

	pthread_mutex_lock(&g_endpoint_lock);
	TAILQ_FOREACH_SAFE(endpoint, &g_endpoint, link, tmp) {
		if (!strncmp(name, endpoint->name, SPDK_VFU_MAX_NAME_LEN)) {
			exist = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_endpoint_lock);

	if (exist) {
		return endpoint;
	}
	return NULL;
}

static int
tgt_vfu_ctx_poller(void *ctx)
{
	struct spdk_vfu_endpoint *endpoint = ctx;
	vfu_ctx_t *vfu_ctx = endpoint->vfu_ctx;
	int ret;

	ret = vfu_run_ctx(vfu_ctx);
	if (spdk_unlikely(ret == -1)) {
		if (errno == EBUSY) {
			return SPDK_POLLER_IDLE;
		}

		if (errno == ENOTCONN) {
			spdk_poller_unregister(&endpoint->vfu_ctx_poller);
			if (endpoint->ops.detach_device) {
				endpoint->ops.detach_device(endpoint);
			}
			endpoint->is_attached = false;
			return SPDK_POLLER_BUSY;
		}
	}

	return ret != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
tgt_accept_poller(void *ctx)
{
	struct spdk_vfu_endpoint *endpoint = ctx;
	int ret;

	if (endpoint->is_attached) {
		return SPDK_POLLER_IDLE;
	}

	ret = vfu_attach_ctx(endpoint->vfu_ctx);
	if (ret == 0) {
		ret = endpoint->ops.attach_device(endpoint);
		if (!ret) {
			SPDK_NOTICELOG("%s: attached successfully\n", spdk_vfu_get_endpoint_id(endpoint));
			/* Polling socket too frequently will cause performance issue */
			endpoint->vfu_ctx_poller = SPDK_POLLER_REGISTER(tgt_vfu_ctx_poller, endpoint, 1000);
			endpoint->is_attached = true;
		}
		return SPDK_POLLER_BUSY;
	}

	if (errno == EAGAIN || errno == EWOULDBLOCK) {
		return SPDK_POLLER_IDLE;
	}

	return SPDK_POLLER_BUSY;
}

static void
tgt_log_cb(vfu_ctx_t *vfu_ctx, int level, char const *msg)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);

	if (level >= LOG_DEBUG) {
		SPDK_DEBUGLOG(vfu, "%s: %s\n", spdk_vfu_get_endpoint_id(endpoint), msg);
	} else if (level >= LOG_INFO) {
		SPDK_INFOLOG(vfu, "%s: %s\n", spdk_vfu_get_endpoint_id(endpoint), msg);
	} else if (level >= LOG_NOTICE) {
		SPDK_NOTICELOG("%s: %s\n", spdk_vfu_get_endpoint_id(endpoint), msg);
	} else if (level >= LOG_WARNING) {
		SPDK_WARNLOG("%s: %s\n", spdk_vfu_get_endpoint_id(endpoint), msg);
	} else {
		SPDK_ERRLOG("%s: %s\n", spdk_vfu_get_endpoint_id(endpoint), msg);
	}
}

static int
tgt_get_log_level(void)
{
	int level;

	if (SPDK_DEBUGLOG_FLAG_ENABLED("vfu")) {
		return LOG_DEBUG;
	}

	level = spdk_log_to_syslog_level(spdk_log_get_level());
	if (level < 0) {
		return LOG_ERR;
	}

	return level;
}

static void
init_pci_config_space(vfu_pci_config_space_t *p, uint16_t ipin)
{
	/* MLBAR */
	p->hdr.bars[0].raw = 0x0;
	/* MUBAR */
	p->hdr.bars[1].raw = 0x0;

	/* vendor specific, let's set them to zero for now */
	p->hdr.bars[3].raw = 0x0;
	p->hdr.bars[4].raw = 0x0;
	p->hdr.bars[5].raw = 0x0;

	/* enable INTx */
	p->hdr.intr.ipin = ipin;
}

static void
tgt_memory_region_add_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);
	void *map_start, *map_end;
	int ret;

	if (!info->vaddr) {
		return;
	}

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(vfu, "Invalid memory region vaddr %p, IOVA %p-%p\n",
			      info->vaddr, map_start, map_end);
		return;
	}

	if (info->prot == (PROT_WRITE | PROT_READ)) {
		ret = spdk_mem_register(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region register %p-%p failed, ret=%d\n",
				    map_start, map_end, ret);
		}
	}

	if (endpoint->ops.post_memory_add) {
		endpoint->ops.post_memory_add(endpoint, map_start, map_end);
	}
}

static void
tgt_memory_region_remove_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);
	void *map_start, *map_end;
	int ret = 0;

	if (!info->vaddr) {
		return;
	}

	map_start = info->mapping.iov_base;
	map_end = info->mapping.iov_base + info->mapping.iov_len;

	if (((uintptr_t)info->mapping.iov_base & MASK_2MB) ||
	    (info->mapping.iov_len & MASK_2MB)) {
		SPDK_DEBUGLOG(vfu, "Invalid memory region vaddr %p, IOVA %p-%p\n",
			      info->vaddr, map_start, map_end);
		return;
	}

	if (endpoint->ops.pre_memory_remove) {
		endpoint->ops.pre_memory_remove(endpoint, map_start, map_end);
	}

	if (info->prot == (PROT_WRITE | PROT_READ)) {
		ret = spdk_mem_unregister(info->mapping.iov_base, info->mapping.iov_len);
		if (ret) {
			SPDK_ERRLOG("Memory region unregister %p-%p failed, ret=%d\n",
				    map_start, map_end, ret);
		}
	}
}

static int
tgt_device_quiesce_cb(vfu_ctx_t *vfu_ctx)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);
	int ret;

	assert(endpoint->ops.quiesce_device);
	ret = endpoint->ops.quiesce_device(endpoint);
	if (ret) {
		errno = EBUSY;
		ret = -1;
	}

	return ret;
}

static int
tgt_device_reset_cb(vfu_ctx_t *vfu_ctx, vfu_reset_type_t type)
{
	struct spdk_vfu_endpoint *endpoint = vfu_get_private(vfu_ctx);

	SPDK_DEBUGLOG(vfu, "Device reset type %u\n", type);

	assert(endpoint->ops.reset_device);
	return endpoint->ops.reset_device(endpoint);
}

static int
tgt_endpoint_realize(struct spdk_vfu_endpoint *endpoint)
{
	int ret;
	uint8_t buf[512];
	struct vsc *vendor_cap;
	ssize_t cap_offset;
	uint16_t vendor_cap_idx, cap_size, sparse_mmap_idx;
	struct spdk_vfu_pci_device pci_dev;
	uint8_t region_idx;

	assert(endpoint->ops.get_device_info);
	ret = endpoint->ops.get_device_info(endpoint, &pci_dev);
	if (ret) {
		SPDK_ERRLOG("%s: failed to get pci device info\n", spdk_vfu_get_endpoint_id(endpoint));
		return ret;
	}

	endpoint->vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, endpoint->uuid, LIBVFIO_USER_FLAG_ATTACH_NB,
					   endpoint, VFU_DEV_TYPE_PCI);
	if (endpoint->vfu_ctx == NULL) {
		SPDK_ERRLOG("%s: error creating libvfio-user context\n", spdk_vfu_get_endpoint_id(endpoint));
		return -EFAULT;
	}
	vfu_setup_log(endpoint->vfu_ctx, tgt_log_cb, tgt_get_log_level());

	ret = vfu_pci_init(endpoint->vfu_ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to initialize PCI\n", endpoint->vfu_ctx);
		goto error;
	}

	vfu_pci_set_id(endpoint->vfu_ctx, pci_dev.id.vid, pci_dev.id.did, pci_dev.id.ssvid,
		       pci_dev.id.ssid);
	vfu_pci_set_class(endpoint->vfu_ctx, pci_dev.class.bcc, pci_dev.class.scc, pci_dev.class.pi);

	/* Add Vendor Capabilities */
	for (vendor_cap_idx = 0; vendor_cap_idx < pci_dev.nr_vendor_caps; vendor_cap_idx++) {
		memset(buf, 0, sizeof(buf));
		cap_size = endpoint->ops.get_vendor_capability(endpoint, buf, 256, vendor_cap_idx);
		if (cap_size) {
			vendor_cap = (struct vsc *)buf;
			assert(vendor_cap->hdr.id == PCI_CAP_ID_VNDR);
			assert(vendor_cap->size == cap_size);

			cap_offset = vfu_pci_add_capability(endpoint->vfu_ctx, 0, 0, vendor_cap);
			if (cap_offset < 0) {
				SPDK_ERRLOG("vfu_ctx %p failed add vendor capability\n", endpoint->vfu_ctx);
				ret = -EFAULT;
				goto error;
			}
		}
	}

	/* Add Standard PCI Capabilities */
	cap_offset = vfu_pci_add_capability(endpoint->vfu_ctx, 0, 0, &pci_dev.pmcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add pmcap\n", endpoint->vfu_ctx);
		ret = -EFAULT;
		goto error;
	}
	SPDK_DEBUGLOG(vfu, "%s PM cap_offset %ld\n", spdk_vfu_get_endpoint_id(endpoint), cap_offset);

	cap_offset = vfu_pci_add_capability(endpoint->vfu_ctx, 0, 0, &pci_dev.pxcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add pxcap\n", endpoint->vfu_ctx);
		ret = -EFAULT;
		goto error;
	}
	SPDK_DEBUGLOG(vfu, "%s PX cap_offset %ld\n", spdk_vfu_get_endpoint_id(endpoint), cap_offset);

	cap_offset = vfu_pci_add_capability(endpoint->vfu_ctx, 0, 0, &pci_dev.msixcap);
	if (cap_offset < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed add msixcap\n", endpoint->vfu_ctx);
		ret = -EFAULT;
		goto error;
	}
	SPDK_DEBUGLOG(vfu, "%s MSIX cap_offset %ld\n", spdk_vfu_get_endpoint_id(endpoint), cap_offset);

	/* Setup PCI Regions */
	for (region_idx = 0; region_idx < VFU_PCI_DEV_NUM_REGIONS; region_idx++) {
		struct spdk_vfu_pci_region *region = &pci_dev.regions[region_idx];
		struct iovec sparse_mmap[SPDK_VFU_MAXIMUM_SPARSE_MMAP_REGIONS];
		if (!region->len) {
			continue;
		}

		if (region->nr_sparse_mmaps) {
			assert(region->nr_sparse_mmaps <= SPDK_VFU_MAXIMUM_SPARSE_MMAP_REGIONS);
			for (sparse_mmap_idx = 0; sparse_mmap_idx < region->nr_sparse_mmaps; sparse_mmap_idx++) {
				sparse_mmap[sparse_mmap_idx].iov_base = (void *)region->mmaps[sparse_mmap_idx].offset;
				sparse_mmap[sparse_mmap_idx].iov_len = region->mmaps[sparse_mmap_idx].len;
			}
		}

		ret = vfu_setup_region(endpoint->vfu_ctx, region_idx, region->len, region->access_cb, region->flags,
				       region->nr_sparse_mmaps ? sparse_mmap : NULL, region->nr_sparse_mmaps,
				       region->fd, region->offset);
		if (ret) {
			SPDK_ERRLOG("vfu_ctx %p failed to setup region %u\n", endpoint->vfu_ctx, region_idx);
			goto error;
		}
		SPDK_DEBUGLOG(vfu, "%s: region %u, len 0x%"PRIx64", callback %p, nr sparse mmaps %u, fd %d\n",
			      spdk_vfu_get_endpoint_id(endpoint), region_idx, region->len, region->access_cb,
			      region->nr_sparse_mmaps, region->fd);
	}

	ret = vfu_setup_device_dma(endpoint->vfu_ctx, tgt_memory_region_add_cb,
				   tgt_memory_region_remove_cb);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup dma callback\n", endpoint->vfu_ctx);
		goto error;
	}

	if (endpoint->ops.reset_device) {
		ret = vfu_setup_device_reset_cb(endpoint->vfu_ctx, tgt_device_reset_cb);
		if (ret < 0) {
			SPDK_ERRLOG("vfu_ctx %p failed to setup reset callback\n", endpoint->vfu_ctx);
			goto error;
		}
	}

	if (endpoint->ops.quiesce_device) {
		vfu_setup_device_quiesce_cb(endpoint->vfu_ctx, tgt_device_quiesce_cb);
	}

	ret = vfu_setup_device_nr_irqs(endpoint->vfu_ctx, VFU_DEV_INTX_IRQ, pci_dev.nr_int_irqs);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup INTX\n", endpoint->vfu_ctx);
		goto error;
	}

	ret = vfu_setup_device_nr_irqs(endpoint->vfu_ctx, VFU_DEV_MSIX_IRQ, pci_dev.nr_msix_irqs);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to setup MSIX\n", endpoint->vfu_ctx);
		goto error;
	}

	ret = vfu_realize_ctx(endpoint->vfu_ctx);
	if (ret < 0) {
		SPDK_ERRLOG("vfu_ctx %p failed to realize\n", endpoint->vfu_ctx);
		goto error;
	}

	endpoint->pci_config_space = vfu_pci_get_config_space(endpoint->vfu_ctx);
	assert(endpoint->pci_config_space != NULL);
	init_pci_config_space(endpoint->pci_config_space, pci_dev.intr_ipin);

	assert(cap_offset != 0);
	endpoint->msix = (struct msixcap *)((uint8_t *)endpoint->pci_config_space + cap_offset);

	return 0;

error:
	if (endpoint->vfu_ctx) {
		vfu_destroy_ctx(endpoint->vfu_ctx);
	}
	return ret;
}

static int
vfu_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask)
{
	int rc;
	struct spdk_cpuset negative_vfu_mask;

	if (cpumask == NULL) {
		return -1;
	}

	if (mask == NULL) {
		spdk_cpuset_copy(cpumask, &g_tgt_core_mask);
		return 0;
	}

	rc = spdk_cpuset_parse(cpumask, mask);
	if (rc < 0) {
		SPDK_ERRLOG("invalid cpumask %s\n", mask);
		return -1;
	}

	spdk_cpuset_copy(&negative_vfu_mask, &g_tgt_core_mask);
	spdk_cpuset_negate(&negative_vfu_mask);
	spdk_cpuset_and(&negative_vfu_mask, cpumask);

	if (spdk_cpuset_count(&negative_vfu_mask) != 0) {
		SPDK_ERRLOG("one of selected cpu is outside of core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_tgt_core_mask));
		return -1;
	}

	spdk_cpuset_and(cpumask, &g_tgt_core_mask);

	if (spdk_cpuset_count(cpumask) == 0) {
		SPDK_ERRLOG("no cpu is selected among core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_tgt_core_mask));
		return -1;
	}

	return 0;
}

static void
vfu_fini_cb(void *arg1)
{
	spdk_vfu_fini_cb fini_cb = arg1;

	fini_cb();
}

static void
tgt_endpoint_start_thread(void *arg1)
{
	struct spdk_vfu_endpoint *endpoint = arg1;

	endpoint->accept_poller = SPDK_POLLER_REGISTER(tgt_accept_poller, endpoint, 1000);
	assert(endpoint->accept_poller != NULL);
}

static void
tgt_endpoint_thread_try_exit(void *arg1)
{
	struct spdk_vfu_endpoint *endpoint = arg1;
	spdk_vfu_fini_cb fini_cb;
	int res;

	res = endpoint->ops.destruct(endpoint);
	if (res == -EAGAIN) {
		/* Let's retry */
		spdk_thread_send_msg(endpoint->thread, tgt_endpoint_thread_try_exit, endpoint);
		return;
	} else if (res) {
		/* We're ignoring this error for now as we have nothing to do with it */
		SPDK_ERRLOG("Endpoint destruct failed with %d\n", res);
	}

	free(endpoint);

	pthread_mutex_lock(&g_endpoint_lock);
	if (g_fini_cb) { /* called due to spdk_vfu_fini() */
		g_fini_endpoint_cnt--;
		if (!g_fini_endpoint_cnt) {
			fini_cb = g_fini_cb;
			g_fini_cb = NULL;
			spdk_thread_send_msg(spdk_thread_get_app_thread(), vfu_fini_cb, fini_cb);
		}
	}
	pthread_mutex_unlock(&g_endpoint_lock);

	spdk_thread_exit(spdk_get_thread());
}

static void
tgt_endpoint_thread_exit(void *arg1)
{
	struct spdk_vfu_endpoint *endpoint = arg1;

	spdk_poller_unregister(&endpoint->accept_poller);
	spdk_poller_unregister(&endpoint->vfu_ctx_poller);

	/* Ensure the attached device is stopped before destroying the vfu context */
	if (endpoint->ops.detach_device) {
		endpoint->ops.detach_device(endpoint);
	}

	if (endpoint->vfu_ctx) {
		vfu_destroy_ctx(endpoint->vfu_ctx);
	}

	tgt_endpoint_thread_try_exit(endpoint);
}

int
spdk_vfu_create_endpoint(const char *endpoint_name, const char *cpumask_str,
			 const char *dev_type_name)
{
	char *basename;
	char uuid[PATH_MAX] = "";
	struct spdk_cpuset cpumask = {};
	struct spdk_vfu_endpoint *endpoint;
	struct spdk_vfu_endpoint_ops *ops;
	int ret = 0;

	ret = vfu_parse_core_mask(cpumask_str, &cpumask);
	if (ret) {
		return ret;
	}

	if (strlen(endpoint_name) >= SPDK_VFU_MAX_NAME_LEN - 1) {
		return -ENAMETOOLONG;
	}

	if (spdk_vfu_get_endpoint_by_name(endpoint_name)) {
		SPDK_ERRLOG("%s already exist\n", endpoint_name);
		return -EEXIST;
	}

	/* Find supported PCI device type */
	ops = tgt_get_pci_device_ops(dev_type_name);
	if (!ops) {
		SPDK_ERRLOG("Request %s device type isn't registered\n", dev_type_name);
		return -ENOTSUP;
	}

	basename = tgt_get_base_path();
	if (snprintf(uuid, sizeof(uuid), "%s%s", basename, endpoint_name) >= (int)sizeof(uuid)) {
		SPDK_ERRLOG("Resulting socket path for endpoint %s is too long: %s%s\n",
			    endpoint_name, basename, endpoint_name);
		return -EINVAL;
	}

	endpoint = calloc(1, sizeof(*endpoint));
	if (!endpoint) {
		return -ENOMEM;
	}

	endpoint->endpoint_ctx = ops->init(endpoint, basename, endpoint_name);
	if (!endpoint->endpoint_ctx) {
		free(endpoint);
		return -EINVAL;
	}
	endpoint->ops = *ops;
	snprintf(endpoint->name, SPDK_VFU_MAX_NAME_LEN, "%s", endpoint_name);
	snprintf(endpoint->uuid, sizeof(uuid), "%s", uuid);

	SPDK_DEBUGLOG(vfu, "Construct endpoint %s\n", endpoint_name);
	/* Endpoint realize */
	ret = tgt_endpoint_realize(endpoint);
	if (ret) {
		endpoint->ops.destruct(endpoint);
		free(endpoint);
		return ret;
	}

	endpoint->thread = spdk_thread_create(endpoint_name, &cpumask);
	if (!endpoint->thread) {
		endpoint->ops.destruct(endpoint);
		vfu_destroy_ctx(endpoint->vfu_ctx);
		free(endpoint);
		return -EFAULT;
	}

	ret = 0;
	pthread_mutex_lock(&g_endpoint_lock);
	if (!g_fini_cb) {
		TAILQ_INSERT_TAIL(&g_endpoint, endpoint, link);
	} else { /* spdk_vfu_fini has been called */
		ret = -EPERM;
	}
	pthread_mutex_unlock(&g_endpoint_lock);

	if (ret) {
		/* we're in the process of destruction, no new endpoint creation is allowed */
		spdk_thread_destroy(endpoint->thread);
		endpoint->ops.destruct(endpoint);
		vfu_destroy_ctx(endpoint->vfu_ctx);
		free(endpoint);
		return -EFAULT;
	}

	spdk_thread_send_msg(endpoint->thread, tgt_endpoint_start_thread, endpoint);

	return 0;
}

int
spdk_vfu_delete_endpoint(const char *endpoint_name)
{
	struct spdk_vfu_endpoint *endpoint;

	endpoint = spdk_vfu_get_endpoint_by_name(endpoint_name);
	if (!endpoint) {
		SPDK_ERRLOG("%s doesn't exist\n", endpoint_name);
		return -ENOENT;
	}

	SPDK_NOTICELOG("Destruct endpoint %s\n", endpoint_name);

	pthread_mutex_lock(&g_endpoint_lock);
	TAILQ_REMOVE(&g_endpoint, endpoint, link);
	pthread_mutex_unlock(&g_endpoint_lock);
	spdk_thread_send_msg(endpoint->thread, tgt_endpoint_thread_exit, endpoint);

	return 0;
}

const char *
spdk_vfu_get_endpoint_id(struct spdk_vfu_endpoint *endpoint)
{
	return endpoint->uuid;
}

const char *
spdk_vfu_get_endpoint_name(struct spdk_vfu_endpoint *endpoint)
{
	return endpoint->name;
}

vfu_ctx_t *
spdk_vfu_get_vfu_ctx(struct spdk_vfu_endpoint *endpoint)
{
	return endpoint->vfu_ctx;
}

void *
spdk_vfu_get_endpoint_private(struct spdk_vfu_endpoint *endpoint)
{
	return endpoint->endpoint_ctx;
}

bool
spdk_vfu_endpoint_msix_enabled(struct spdk_vfu_endpoint *endpoint)
{
	return endpoint->msix->mxc.mxe;
}

bool
spdk_vfu_endpoint_intx_enabled(struct spdk_vfu_endpoint *endpoint)
{
	return !endpoint->pci_config_space->hdr.cmd.id;
}

void *
spdk_vfu_endpoint_get_pci_config(struct spdk_vfu_endpoint *endpoint)
{
	return (void *)endpoint->pci_config_space;
}

void
spdk_vfu_init(spdk_vfu_init_cb init_cb)
{
	uint32_t i;
	size_t len;

	if (g_endpoint_path_dirname[0] == '\0') {
		if (getcwd(g_endpoint_path_dirname, sizeof(g_endpoint_path_dirname) - 2) == NULL) {
			SPDK_ERRLOG("getcwd failed\n");
			init_cb(-errno);
			return;
		}

		len = strlen(g_endpoint_path_dirname);
		if (g_endpoint_path_dirname[len - 1] != '/') {
			g_endpoint_path_dirname[len] = '/';
			g_endpoint_path_dirname[len + 1] = '\0';
		}
	}

	spdk_cpuset_zero(&g_tgt_core_mask);
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_set_cpu(&g_tgt_core_mask, i, true);
	}

	init_cb(0);
}

void *
spdk_vfu_map_one(struct spdk_vfu_endpoint *endpoint, uint64_t addr, uint64_t len, dma_sg_t *sg,
		 struct iovec *iov,
		 int prot)
{
	int ret;

	assert(endpoint != NULL);
	assert(endpoint->vfu_ctx != NULL);
	assert(sg != NULL);
	assert(iov != NULL);

	ret = vfu_addr_to_sgl(endpoint->vfu_ctx, (void *)(uintptr_t)addr, len, sg, 1, prot);
	if (ret < 0) {
		return NULL;
	}

	ret = vfu_sgl_get(endpoint->vfu_ctx, sg, iov, 1, 0);
	if (ret != 0) {
		return NULL;
	}

	assert(iov->iov_base != NULL);
	return iov->iov_base;
}

void
spdk_vfu_unmap_sg(struct spdk_vfu_endpoint *endpoint, dma_sg_t *sg, struct iovec *iov, int iovcnt)
{
	assert(endpoint != NULL);
	assert(endpoint->vfu_ctx != NULL);
	assert(sg != NULL);
	assert(iov != NULL);

	vfu_sgl_put(endpoint->vfu_ctx, sg, iov, iovcnt);
}

void
spdk_vfu_fini(spdk_vfu_fini_cb fini_cb)
{
	struct spdk_vfu_endpoint *endpoint, *tmp;
	struct tgt_pci_device_ops *ops, *ops_tmp;
	uint32_t endpoint_cnt = 0;

	assert(spdk_thread_is_app_thread(NULL));

	pthread_mutex_lock(&g_endpoint_lock);
	assert(!g_fini_cb);
	TAILQ_FOREACH_SAFE(ops, &g_pci_device_ops, link, ops_tmp) {
		TAILQ_REMOVE(&g_pci_device_ops, ops, link);
		free(ops);
	}

	TAILQ_FOREACH_SAFE(endpoint, &g_endpoint, link, tmp) {
		TAILQ_REMOVE(&g_endpoint, endpoint, link);
		endpoint_cnt++;
		spdk_thread_send_msg(endpoint->thread, tgt_endpoint_thread_exit, endpoint);
	}

	/* NOTE: g_fini_cb and g_fini_endpoint_cnt are accessed under the same mutex so it's safe to assign them here */
	if (endpoint_cnt) {
		g_fini_endpoint_cnt = endpoint_cnt;
		g_fini_cb = fini_cb;
	}
	pthread_mutex_unlock(&g_endpoint_lock);

	if (!endpoint_cnt) {
		fini_cb();
	}
}
SPDK_LOG_REGISTER_COMPONENT(vfu)
