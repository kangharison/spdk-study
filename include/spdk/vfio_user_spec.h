/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] vfio-user 와이어 프로토콜(메시지 포맷) 정의 (vfio_user_spec.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 vfio-user 프로토콜의 "와이어 포맷" — 즉 client 와 server 가
 * Unix 도메인 소켓을 통해 주고받는 메시지 헤더와 명령 코드, 페이로드
 * 구조체들을 정의한다. vfio-user 는 리눅스 커널 VFIO 프레임워크의 ioctl
 * 인터페이스(VFIO_DEVICE_GET_INFO, VFIO_DEVICE_GET_REGION_INFO,
 * VFIO_IOMMU_MAP_DMA, …)를 거의 그대로 메시지로 옮긴 것이라, 본 파일의
 * 구조체 다수가 커널 uapi 의 vfio_* 구조체에 대응한다(주석에 "based on …"
 * 명시). 이 헤더 하나로 client/server 양측이 동일한 정의를 공유하므로
 * 본 파일의 모든 구조체는 packed 정렬과 ABI 안정성이 핵심이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 상으로는 데이터 구조 정의이므로 "호출되지 않고 인클루드되는"
 * 위치이다. 사용 측면의 흐름은:
 *   client (SPDK lib/nvme/nvme_vfio_user.c)
 *     → lib/vfio_user/host/vfio_user.c 가 본 헤더의 enum vfio_user_command
 *       값을 사용해 메시지를 만들고 socket 으로 송신
 *     ↔ Unix 도메인 소켓 (SOCK_STREAM)
 *       ↔ server (libvfio-user / SPDK NVMe-vfio target / qemu vhost-user-blk
 *         등)이 동일한 헤더로 메시지를 파싱하고 응답
 *
 * 핸드셰이크/통상 흐름:
 *   1) VERSION 교환 → 2) DMA_MAP 으로 client 의 hugepage 영역 등록
 *   → 3) DEVICE_GET_INFO/REGION_INFO/IRQ_INFO 로 디바이스 능력 프로빙
 *   → 4) REGION_READ/WRITE 로 BAR(NVMe MMIO 도어벨/CC/CSTS) 접근
 *   → 5) DEVICE_SET_IRQS 로 MSI-X 등록 → 6) VM_INTERRUPT(여기서는 서버에서
 *   client 로의 인터럽트 통지) 수신 → 종료 시 DMA_UNMAP / 소켓 close.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h (uint16_t/uint32_t/uint64_t 등). 그 외 SPDK
 *   내부 의존성은 없다 — spec 헤더는 어디든 가볍게 인클루드 가능.
 * - 의존자: include/spdk/vfio_user_pci.h (client 헬퍼 API),
 *   lib/vfio_user/host/* (client 송수신 구현), lib/nvme/nvme_vfio_user.c
 *   (NVMe 드라이버의 vfio-user 트랜스포트), 그리고 server 측 SPDK
 *   타깃(예: nvmf vfio-user transport).
 * - 데이터 흐름: 본 헤더의 구조체는 메모리 그대로 socket 에 write 되어
 *   상대 프로세스로 전달된다. 따라서 모든 구조체에 __attribute__((packed))
 *   가 붙고 little-endian / 8바이트 정렬이 가정된다. 보조 페이로드는
 *   가변 길이(data[]) 로 헤더 뒤에 이어 붙는다.
 * - 공유 자료구조: 커널 VFIO 의 uapi 구조체와 1:1 호환되도록 설계 —
 *   필드 추가 시에도 끝에만 append (argsz 필드로 ABI 호환).
 *
 * === 주요 함수/구조체 요약 ===
 * - enum vfio_user_command: 명령 코드 (VERSION=1 ... DIRTY_PAGES=14).
 *   각 메시지 헤더의 cmd 필드로 들어감.
 * - enum vfio_user_message_type: COMMAND(0)/REPLY(1).
 * - struct vfio_user_header: 모든 메시지의 공통 16바이트 헤더 — msg_id,
 *   cmd, msg_size, flags(타입/no_reply/error), error_no.
 * - struct vfio_user_version: VERSION 핸드셰이크 페이로드 (major/minor +
 *   JSON capabilities).
 * - struct vfio_user_device_info: DEVICE_GET_INFO 응답 — num_regions/num_irqs.
 * - struct vfio_user_dma_map / dma_unmap: DMA 영역(주로 hugepage 메모리)
 *   등록/해제. 커널 VFIO 의 vfio_iommu_type1_dma_map 의 미러.
 * - struct vfio_user_region_access: REGION_READ/WRITE 페이로드 — BAR
 *   접근. NVMe 도어벨 라이트가 여기로 옴.
 * - struct vfio_user_dma_region_access: DMA_READ/WRITE 페이로드 —
 *   server 가 client 의 등록된 메모리에서 직접 데이터를 읽거나 쓸 때 사용.
 * - struct vfio_user_irq_info / bitmap / bitmap_range: IRQ 와 dirty page
 *   tracking 관련 보조 구조체.
 */

#ifndef _VFIO_USER_SPEC_H
#define _VFIO_USER_SPEC_H

#include "spdk/stdinc.h"
/* [한국어] uint16_t/uint32_t/uint64_t 등 stdint 타입과 __attribute__ 매크로
 * 사용에 필요한 최소 의존성. spec 헤더라 가능한 의존성을 줄여 server/client
 * 양쪽에서 가볍게 인클루드되도록 한다. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러로 인클루드되는 경우에도 C 링크리지 보장. */
#endif

/*
 * [한국어]
 * enum vfio_user_command - vfio-user 메시지의 명령 코드(opcode)
 *
 * 이 enum 의 값은 그대로 vfio_user_header.cmd 필드에 실린다. 따라서 일단
 * 정의되면 값을 바꿀 수 없는 ABI 의 일부이다 — 새 명령 추가 시 끝에만
 * append 하고 VFIO_USER_MAX 만 갱신한다. 의미는 커널 VFIO 의 ioctl 과
 * 1:1 대응한다 (예: VFIO_USER_DEVICE_GET_INFO ↔ VFIO_DEVICE_GET_INFO ioctl).
 *
 * 클라이언트→서버 / 서버→클라이언트 방향 구분은 메시지 헤더 flags.type 으로
 * 표현되며, 이 enum 자체는 방향에 무관하다 (예: DMA_READ/WRITE 는 서버가
 * 클라이언트에 보내는 명령이지만 같은 enum 사용).
 */
enum vfio_user_command {
	VFIO_USER_VERSION			= 1,
	/* [한국어] 프로토콜 버전 협상 핸드셰이크. 연결 직후 client 가 server 에
	 * 자신의 major/minor 와 JSON capabilities (예: 지원하는 migration,
	 * pgsizes, max_msg_fds) 를 보내고, server 가 자신의 능력으로 응답.
	 * 이 단계가 끝나야 다른 명령 사용 가능. */

	VFIO_USER_DMA_MAP			= 2,
	/* [한국어] client 가 server 에 IOVA→host 메모리 매핑을 등록. 페이로드는
	 * struct vfio_user_dma_map + SCM_RIGHTS 로 전달되는 memfd. SPDK 에서는
	 * env_dpdk hugepage segment 들이 여기서 등록되어 server 가 zero-copy
	 * DMA 로 읽고 쓸 수 있게 된다. 커널 VFIO 의 VFIO_IOMMU_MAP_DMA 와 등가. */

	VFIO_USER_DMA_UNMAP			= 3,
	/* [한국어] DMA_MAP 의 역. dirty bitmap 을 함께 가져올 수 있는 플래그
	 * (VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP) 가 정의되어 있어 라이브 마이그레이션
	 * 시 page-modification 추적과 같이 사용된다. */

	VFIO_USER_DEVICE_GET_INFO		= 4,
	/* [한국어] 디바이스 일반 정보 (flags, num_regions, num_irqs) 조회.
	 * client 의 setup() 시퀀스에서 두 번째로 호출. */

	VFIO_USER_DEVICE_GET_REGION_INFO	= 5,
	/* [한국어] 각 PCI region(BAR0~5, ROM, PCI config space, VGA 등) 의
	 * size/offset/flags 와 sparse mmap 가능 영역 등을 조회. mmap 가능한
	 * region 은 GET_REGION_IO_FDS 로 fd 받아 zero-copy 매핑에 사용. */

	VFIO_USER_DEVICE_GET_REGION_IO_FDS	= 6,
	/* [한국어] mmap 가능한 region 의 SCM_RIGHTS fd 들을 받아오는 명령.
	 * 받은 fd 를 mmap() 하면 client 측 가상주소로 직접 BAR 접근이 가능 —
	 * NVMe 도어벨 라이트의 hot path 에 결정적. */

	VFIO_USER_DEVICE_GET_IRQ_INFO		= 7,
	/* [한국어] 디바이스 IRQ 인덱스 정보(타입: INTx/MSI/MSI-X, 카운트, 플래그)
	 * 조회. NVMe 의 경우 MSI-X 가 일반적. */

	VFIO_USER_DEVICE_SET_IRQS		= 8,
	/* [한국어] IRQ 트리거용 eventfd 등록/마스킹/언마스킹. SCM_RIGHTS 로
	 * eventfd 를 server 에 전달하면 server 가 인터럽트 발생 시 그 fd 에
	 * write() 하여 client 의 epoll/spdk_thread 가 깨어난다. */

	VFIO_USER_REGION_READ			= 9,
	/* [한국어] BAR/region 일부 영역 읽기. mmap 불가하거나 sparse 영역 외부일
	 * 때 사용. NVMe 컨트롤러 레지스터 폴링 등에 사용 가능 (단, hot path 는
	 * 가능한 mmap 사용). 페이로드: struct vfio_user_region_access. */

	VFIO_USER_REGION_WRITE			= 10,
	/* [한국어] BAR/region 일부 영역 쓰기. NVMe 도어벨 발사가 여기서
	 * 메시지로 변환되는 경로 (mmap 불가 시). 페이로드 동일. */

	VFIO_USER_DMA_READ			= 11,
	/* [한국어] server → client 방향 명령. server 가 등록된 client 메모리에서
	 * 일부 영역을 읽고 싶을 때 사용. 일반적으로 server 가 fd 로 직접
	 * mmap 했다면 불필요하지만, 매핑이 불가/거부된 경우의 폴백. */

	VFIO_USER_DMA_WRITE			= 12,
	/* [한국어] DMA_READ 의 쓰기 버전 — server 가 client 메모리에 데이터를
	 * 쓸 때 사용. 대상 메모리는 사전에 DMA_MAP 으로 등록되어 있어야 한다. */

	VFIO_USER_DEVICE_RESET			= 13,
	/* [한국어] 디바이스 reset 요청. NVMe 의 경우 컨트롤러 reset 과 매핑되며,
	 * 모든 SQ/CQ 폐기 + admin queue 재초기화가 뒤따른다. */

	VFIO_USER_DIRTY_PAGES			= 14,
	/* [한국어] 라이브 마이그레이션을 위한 dirty page tracking 시작/중지/조회.
	 * 페이로드는 struct vfio_user_bitmap_range 와 비슷한 형태. */

	VFIO_USER_MAX,
	/* [한국어] sentinel — 정의된 명령 수의 상한. 배열 크기 산정과 인자 검증에
	 * 사용. 새 명령 추가 시 이 위에 append 한다. */
};

/*
 * [한국어]
 * enum vfio_user_message_type - 메시지 방향성 (요청/응답)
 *
 * 헤더 flags.type 필드(4비트)에 들어가는 값. command(0) 가 요청, reply(1)
 * 가 응답이다. 이 enum 자체는 헤더 매크로 VFIO_USER_F_TYPE_COMMAND/REPLY
 * 와 값이 일치하도록 설계되어 있다.
 */
enum vfio_user_message_type {
	VFIO_USER_MESSAGE_COMMAND	= 0,
	/* [한국어] 요청 메시지 — 송신측이 응답을 기다리는 일반 명령. */

	VFIO_USER_MESSAGE_REPLY		= 1,
	/* [한국어] 응답 메시지 — 같은 msg_id 를 갖는 COMMAND 의 결과. flags.error
	 * 가 1 이면 error_no 에 errno 가 들어 있다. */
};

#define VFIO_USER_FLAGS_NO_REPLY	(0x1)
/* [한국어] 송신측이 응답을 요구하지 않는다는 힌트(이론상 server→client
 * 비동기 통지 등에 사용 가능). 헤더 flags.no_reply 비트와는 별개의
 * "고수준 매크로" — 호출자가 헤더를 채울 때 편의 상수로 사용. */

/*
 * [한국어]
 * struct vfio_user_header - 모든 vfio-user 메시지의 공통 헤더 (16바이트, packed)
 *
 * 송신측은 이 헤더 다음에 명령별 페이로드(struct vfio_user_dma_map 등)를
 * 이어 붙여 socket write 한다. 수신측은 헤더의 msg_size 만큼 read 해서
 * 페이로드 전체를 한 번에 처리한다. msg_id 는 요청-응답 매칭에 쓰이며,
 * 응답은 동일한 msg_id 를 그대로 채워 보낸다.
 */
struct vfio_user_header {
	uint16_t	msg_id;
	/* [한국어] 메시지 식별자. 송신측이 단조 증가시키며, 수신측은 응답에
	 * 동일 값을 실어 보낸다. 동시에 여러 요청을 in-flight 로 둘 수 있게
	 * 해주지만, 현재 SPDK 구현은 직렬화하여 보낸다. */

	uint16_t	cmd;
	/* [한국어] enum vfio_user_command 값 (1~14). 응답에서도 원 요청과 같은
	 * cmd 값을 그대로 둔다 (REPLY 임은 flags.type 으로 구분). */

	uint32_t	msg_size;
	/* [한국어] 헤더 + 페이로드의 총 바이트 수. 수신측이 한 메시지 경계를
	 * 알기 위한 길이 필드. SOCK_STREAM 을 쓰므로 자체 길이 프레이밍이 필수. */

	struct {
		uint32_t	type     : 4;
		/* [한국어] 4비트 — 메시지 타입. 0=COMMAND, 1=REPLY (아래 매크로
		 * VFIO_USER_F_TYPE_COMMAND/REPLY 와 일치). 4비트라 미래 확장 여지. */
#define VFIO_USER_F_TYPE_COMMAND	0
		/* [한국어] flags.type 가 COMMAND 임을 나타내는 매크로 상수. */
#define VFIO_USER_F_TYPE_REPLY		1
		/* [한국어] flags.type 가 REPLY 임을 나타내는 매크로 상수. */

		uint32_t	no_reply : 1;
		/* [한국어] 1비트 — 송신측이 응답 불요라고 표시 (현재는 거의 미사용).
		 * server→client 비동기 통지에 잠재적으로 활용 가능. */

		uint32_t	error    : 1;
		/* [한국어] 1비트 — 응답 시 에러 발생 여부. 1 이면 error_no 가
		 * 유효한 양수 errno 값. */

		uint32_t	resvd    : 26;
		/* [한국어] 26비트 — 예약. 송신측은 0 으로 채우고 수신측은 무시.
		 * 미래 플래그를 위한 공간. */
	} flags;
	/* [한국어] 32비트 비트필드 묶음. C 비트필드는 컴파일러 의존성이 있지만,
	 * vfio-user 는 GCC/Clang 의 little-endian 정렬에 의존하여 정의되어 있고
	 * __attribute__((packed)) 로 정렬을 강제. */

	uint32_t	error_no;
	/* [한국어] flags.error == 1 일 때 errno 값(EINVAL, EFAULT 등). client 의
	 * spdk_vfio_user_pci_bar_access() 등은 이 값을 그대로 음수화해 반환한다. */
} __attribute__((packed));
/* [한국어] 패딩 없이 정확히 16바이트 — 메모리 그대로 와이어로 흘러간다. */

/*
 * [한국어]
 * struct vfio_user_version - VFIO_USER_VERSION 핸드셰이크 페이로드
 *
 * 헤더 뒤에 이 구조체가 따라오며, data[] 에는 JSON 형식의 capabilities
 * 문자열이 들어간다 (예: {"capabilities":{"max_msg_fds":8,"max_data_xfer_size":..}}).
 * 양측이 자신의 major/minor 와 capabilities 를 교환해서 호환 모드를 결정.
 */
struct vfio_user_version {
	uint16_t	major;
	/* [한국어] 프로토콜 메이저 버전 — 호환 불가능한 변경 시 증가. 현재 0. */

	uint16_t	minor;
	/* [한국어] 프로토콜 마이너 버전 — 후방 호환 가능한 변경 시 증가. 현재 1. */

	uint8_t		data[];
	/* [한국어] flexible array — JSON 형태의 capabilities 문자열. 길이는
	 * 헤더 msg_size 에서 sizeof(header)+sizeof(major)+sizeof(minor) 를 뺀 값. */
} __attribute__((packed));

/*
 * Similar to vfio_device_info, but without caps (yet).
 */
/*
 * [한국어]
 * struct vfio_user_device_info - VFIO_USER_DEVICE_GET_INFO 응답 페이로드
 *
 * 커널 uapi 의 struct vfio_device_info 와 거의 동일하나, capability chain
 * (caps_offset 등)은 아직 미지원이다. client 는 num_regions/num_irqs 만큼
 * 후속 GET_REGION_INFO / GET_IRQ_INFO 를 호출해 디바이스 능력을 모두 파악한다.
 */
struct vfio_user_device_info {
	uint32_t	argsz;
	/* [한국어] 호출자가 알고 있는 구조체 크기 — 버전 호환을 위해 커널
	 * VFIO 와 동일한 ABI 트릭 사용. server 는 client 가 보낸 argsz 보다
	 * 작거나 같은 부분만 채워 응답한다. */

	/* VFIO_DEVICE_FLAGS_* */
	uint32_t	flags;
	/* [한국어] 디바이스 종류/특성 비트. VFIO_DEVICE_FLAGS_PCI(0x2),
	 * FLAGS_RESET(0x1), FLAGS_PLATFORM(0x4) 등. NVMe 의 경우 PCI|RESET. */

	uint32_t	num_regions;
	/* [한국어] 이 디바이스가 노출하는 region 수 (BAR0~5 + PCI config + …).
	 * 일반 NVMe SSD 는 보통 9 (6 BAR + ROM + PCI config + VGA 더미 등). */

	uint32_t	num_irqs;
	/* [한국어] IRQ 인덱스 수. NVMe 의 경우 INTx + MSI + MSI-X + ERR + REQ
	 * 등의 카테고리가 따로 인덱스로 나타난다. */
} __attribute__((packed));

/* based on struct vfio_bitmap */
/*
 * [한국어]
 * struct vfio_user_bitmap - 페이지 단위 비트맵 컨테이너
 *
 * 라이브 마이그레이션의 dirty page 추적과 일부 다른 명령에서 보조로 사용.
 * 커널 uapi vfio_bitmap 의 미러로, pgsize 는 비트 1개당 페이지 크기,
 * size 는 data[] 의 바이트 길이.
 */
struct vfio_user_bitmap {
	uint64_t	pgsize;
	/* [한국어] 비트 한 개가 표현하는 페이지 크기(바이트). 일반적으로
	 * 4096 또는 2 MiB. */

	uint64_t	size;
	/* [한국어] data[] 영역의 바이트 길이. 비트 수 = size * 8. */

	char		data[];
	/* [한국어] flexible array — 실제 비트맵 바이트들. dirty=1, clean=0. */
} __attribute__((packed));

/* based on struct vfio_iommu_type1_dma_map */
/*
 * [한국어]
 * struct vfio_user_dma_map - VFIO_USER_DMA_MAP 페이로드 (IOVA 매핑 등록)
 *
 * client 가 자신의 host-virtual 메모리 영역(주로 SPDK env_dpdk 의 hugepage
 * memseg) 을 server 의 IOVA 공간에 매핑한다. 매핑된 후에는 server 가
 * VFIO_USER_DMA_READ/WRITE 또는 등록된 fd mmap 을 통해 그 메모리를
 * DMA 대상처럼 읽고 쓸 수 있다. SCM_RIGHTS 로 함께 전달되는 fd 가 메모리의
 * 출처(memfd/hugetlbfs fd)이다.
 */
struct vfio_user_dma_map {
	uint32_t	argsz;
	/* [한국어] 호환성을 위한 구조체 크기 (커널 ABI 트릭). */

#define VFIO_USER_F_DMA_REGION_READ	(1 << 0)
	/* [한국어] flags 비트0 — server 가 이 영역을 read 가능. R/W 분리는
	 * IOMMU/페이지 보호와 매핑된다. */
#define VFIO_USER_F_DMA_REGION_WRITE	(1 << 1)
	/* [한국어] flags 비트1 — server 가 이 영역을 write 가능. SPDK NVMe 의
	 * 데이터 버퍼는 보통 R+W 둘 다 설정. */

	uint32_t	flags;
	/* [한국어] 위 R/W 비트 OR. */

	uint64_t	offset;
	/* [한국어] 함께 전달된 fd 내부에서의 시작 오프셋(바이트). hugetlbfs fd
	 * 가 여러 페이지를 담을 때 그중 일부만 매핑할 수 있게 한다. */

	uint64_t	addr;
	/* [한국어] server 측 IOVA 공간에서의 시작 주소. SPDK 는 보통 host VA
	 * 와 같은 값을 IOVA 로 사용 (vfio "iova == VA" 모델). */

	uint64_t	size;
	/* [한국어] 매핑할 영역의 바이트 길이. 페이지 정렬되어야 함. */
} __attribute__((packed));

/* based on struct vfio_iommu_type1_dma_unmap */
/*
 * [한국어]
 * struct vfio_user_dma_unmap - VFIO_USER_DMA_UNMAP 페이로드 (IOVA 매핑 해제)
 *
 * 등록된 영역을 풀고, 옵션으로 그동안의 dirty bitmap 을 함께 받아온다.
 * 라이브 마이그레이션 종료 단계에서 해제와 dirty 수집을 한 번에 한다.
 */
struct vfio_user_dma_unmap {
	uint32_t	argsz;
	/* [한국어] 호환성용 크기. */

#ifndef VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP
#define VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP	(1 << 0)
#endif
	/* [한국어] flags 비트0 — 해제와 함께 dirty bitmap 을 응답에 포함시키도록
	 * 요청. 같은 매크로가 커널 uapi 헤더에서 이미 정의되어 있으면 그걸 사용. */

	uint32_t	flags;
	/* [한국어] 위 비트 OR. */

	uint64_t	addr;
	/* [한국어] 해제할 영역의 시작 IOVA. */

	uint64_t	size;
	/* [한국어] 해제할 영역 길이. */

	struct vfio_user_bitmap	bitmap[];
	/* [한국어] flexible array — GET_DIRTY_BITMAP 플래그가 설정된 경우에 한해
	 * 응답에 채워지는 dirty bitmap. 비활성 시 길이 0. */
};

/*
 * [한국어]
 * struct vfio_user_region_access - VFIO_USER_REGION_READ / REGION_WRITE 페이로드
 *
 * BAR(혹은 기타 region) 의 일부 영역을 읽고 쓰기 위한 헤더. NVMe 도어벨이
 * mmap 불가 region 에 있거나 mmap 폴백 시 한 번 발사할 때마다 이 구조체
 * 하나의 메시지가 클라이언트→서버로 흐른다 (write 의 경우 data[] 에 기록값,
 * read 의 응답에 data[] 로 결과).
 */
struct vfio_user_region_access {
	uint64_t	offset;
	/* [한국어] region 시작 기준 오프셋. NVMe 도어벨이라면 0x1000 + 큐 인덱스
	 * 별 stride 값. */

	uint32_t	region;
	/* [한국어] region 인덱스 (보통 BAR 번호 0~5 + 추가 region 들). */

	uint32_t	count;
	/* [한국어] data[] 의 바이트 길이. NVMe 도어벨은 4 바이트 워드. */

	uint8_t		data[];
	/* [한국어] flexible array — write 시 기록 데이터, read 응답 시 결과 데이터. */
} __attribute__((packed));

/*
 * [한국어]
 * struct vfio_user_dma_region_access - VFIO_USER_DMA_READ / DMA_WRITE 페이로드
 *
 * server → client 방향. 등록된 client 메모리를 직접 mmap 하지 못한 server
 * 가 일부 영역을 메시지로 읽고 쓸 때 사용. SPDK 에서는 보통 mmap 가능한
 * hugepage 를 등록하므로 이 경로는 거의 사용되지 않으나, 호환성을 위해 정의됨.
 */
struct vfio_user_dma_region_access {
	uint64_t	addr;
	/* [한국어] 대상 IOVA — DMA_MAP 으로 등록된 영역 안의 주소여야 함. */

	uint64_t	count;
	/* [한국어] data[] 길이(바이트). 1회 메시지로 전달 가능한 크기는 VERSION
	 * 핸드셰이크의 max_data_xfer_size capability 로 제한된다. */

	uint8_t		data[];
	/* [한국어] flexible array — write 시 보내는 데이터, read 응답 시 결과 데이터. */
} __attribute__((packed));

/*
 * [한국어]
 * struct vfio_user_irq_info - server → client 인터럽트 통지 페이로드
 *
 * server 가 어떤 IRQ subindex 가 발생했음을 알릴 때(또는 SET_IRQS 명령의
 * 일부 변형에서) 사용. 일반적인 SPDK 경로에서는 SET_IRQS 로 등록한 eventfd
 * 가 직접 시그널되므로 본 구조체는 잘 사용되지 않으나, fallback 경로에 존재.
 */
struct vfio_user_irq_info {
	uint32_t	subindex;
	/* [한국어] IRQ 카테고리(MSI-X 등) 내의 벡터 번호. NVMe MSI-X 는 admin/
	 * I/O 큐 별로 다른 subindex 를 갖는다. */
} __attribute__((packed));

/* based on struct vfio_iommu_type1_dirty_bitmap_get */
/*
 * [한국어]
 * struct vfio_user_bitmap_range - dirty page 추적 결과의 범위 + 비트맵
 *
 * VFIO_USER_DIRTY_PAGES (특히 GET_BITMAP 서브 명령) 의 응답 페이로드.
 * iova 부터 size 바이트 영역에 대한 dirty 비트맵을 bitmap.data[] 에 담는다.
 * 라이브 마이그레이션 사전 복사 단계에서 반복적으로 폴링된다.
 */
struct vfio_user_bitmap_range {
	uint64_t	iova;
	/* [한국어] 추적 대상 영역의 시작 IOVA. */

	uint64_t	size;
	/* [한국어] 추적 대상 영역의 길이(바이트). bitmap.size * bitmap.pgsize * 8
	 * 와 일치해야 한다. */

	struct vfio_user_bitmap	bitmap;
	/* [한국어] 페이지별 dirty 비트맵. 위 size 와 pgsize 로부터 비트 수 결정. */
} __attribute__((packed));


#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 종료. */
#endif

#endif
/* [한국어] _VFIO_USER_SPEC_H 헤더 가드 종료. */
