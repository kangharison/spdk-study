/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 */

/*
 * [한국어 설명] vfio-user 클라이언트(host) 측 PCI 헬퍼 공개 API (vfio_user_pci.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 SPDK 가 vfio-user 프로토콜의 "클라이언트(host)" 역할로 동작할 때
 * 원격(또는 동일 호스트의 다른 프로세스에 위치한) PCI 디바이스에 접근하기 위한
 * 작은 헬퍼 API를 외부에 노출한다. vfio-user 프로토콜은 리눅스 커널의 VFIO
 * (Virtual Function I/O) ioctl 기반 사용자공간 드라이버 프레임워크를
 * "Unix 도메인 소켓 위의 메시지" 로 옮긴 것으로, server 가 PCI 디바이스를
 * 에뮬레이트/대리(소유)하고 client 가 BAR 접근, MSI-X 설정, DMA 매핑 등을
 * 메시지로 요청한다.
 *
 * 이 파일에 선언된 함수들은 SPDK 의 lib/nvme/nvme_vfio_user.c 가 NVMe 컨트롤러를
 * vfio-user 트랜스포트로 부착(attach)할 때 사용한다. setup() → BAR/IRQ 정보
 * 획득 → BAR 영역 read/write/매핑 → 종료 시 release() 의 흐름이다. 이로써
 * SPDK 의 NVMe PCIe 드라이버는 실제 커널 VFIO 디바이스와 vfio-user 디바이스를
 * 거의 동일한 인터페이스로 다룰 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 상으로는 다음 위치에 해당한다:
 *   spdk_nvme_probe()/attach()
 *     → lib/nvme/nvme_vfio_user.c (transport == VFIO_USER 분기)
 *       → spdk_vfio_user_setup()      // 본 헤더, 소켓 연결 + VERSION 핸드셰이크
 *       → spdk_vfio_user_get_bar_addr() / spdk_vfio_user_pci_bar_access()
 *                                     // BAR0(NVMe MMIO 레지스터) 접근
 *       → spdk_vfio_user_release()    // 종료 시 정리
 *
 * 즉 SPDK 의 "유저스페이스 NVMe 드라이버" 가 일반 PCIe BDF(0000:81:00.0) 대신
 * Unix 도메인 소켓 경로(예: /var/run/vfio-user.sock) 로 접근하는 경로이다.
 * polled-mode 도어벨 라이트, MSI-X 마스킹, DMA 매핑 등 NVMe 1.x 스펙이 요구하는
 * MMIO 동작이 모두 vfio-user 메시지로 변환되어 전송된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h (uint32_t 등 stdint, size_t), spdk/vfio_user_spec.h
 *   (enum vfio_user_command 정의 — fuzzing 진입점에서 사용).
 * - 의존자: lib/vfio_user/host/vfio_user.c (구현체) 와 그것을 사용하는
 *   lib/nvme/nvme_vfio_user.c. 또한 test/nvme/vfio_user_fuzz/ 등 퍼징 도구.
 * - 데이터 흐름: client 의 BAR 접근 함수 호출 → vfio_user_dev 구조체 내부
 *   소켓을 통해 server 에 VFIO_USER_REGION_READ/WRITE 메시지 송신 → 응답
 *   페이로드를 호출자의 buf 에 채워 반환. DMA 영역 등록(VFIO_USER_DMA_MAP)
 *   은 이 파일이 직접 노출하지는 않고 내부에서 SPDK env_dpdk hugepage
 *   메모리를 server 에 등록한다.
 * - 공유 자료구조: 불투명 핸들 struct vfio_device — 본 헤더에서는
 *   forward declaration 만 노출, 내부 필드는 lib/vfio_user/host/ 에 캡슐화.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_vfio_user_setup(path):  소켓 연결 + VERSION 핸드셰이크 + DEVICE_GET_INFO
 *   + REGION_INFO 수집까지 수행하여 사용 가능한 vfio_device 핸들을 반환.
 * - spdk_vfio_user_release(dev): 위 setup() 의 역과정 — DMA 매핑 해제, 소켓
 *   close, 핸들 free.
 * - spdk_vfio_user_pci_bar_access(): 임의의 BAR 영역에 대해 R/W I/O 수행 —
 *   NVMe MMIO 도어벨 라이트의 백엔드.
 * - spdk_vfio_user_get_bar_addr(): BAR 가 mmap 가능하면 host 가상주소를
 *   반환 (zero-copy 경로).
 * - spdk_vfio_user_dev_send_request(): 임의의 vfio_user_command 를 보낼 수 있는
 *   raw 진입점 — 퍼징/디버깅 전용.
 */

#ifndef _SPDK_VFIO_USER_PCI_H
#define _SPDK_VFIO_USER_PCI_H

#include "spdk/stdinc.h"
/* [한국어] C 표준 정수 타입(uint32_t, uint64_t), size_t, bool 등을 가져오기 위한
 * SPDK 공통 stdinc 헤더. SPDK 는 stdio/stdint/stdbool/string 등을 모아놓은
 * spdk/stdinc.h 를 모든 헤더의 첫 인클루드 후보로 사용한다. */
#include "spdk/vfio_user_spec.h"
/* [한국어] vfio-user 와이어 프로토콜 정의(enum vfio_user_command, struct
 * vfio_user_header 등). 본 헤더는 fuzzing 용 spdk_vfio_user_dev_send_request()
 * 가 enum vfio_user_command 를 인자로 받기 때문에 spec 헤더가 필요하다. */

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 컴파일러로 인클루드되는 경우에도 이름 맹글링 없이(extern "C")
 * 묶어 SPDK 라이브러리(C ABI)와 링크 호환되도록 한다. */
#endif

struct vfio_device;
/* [한국어] vfio-user 디바이스 핸들의 전방선언(forward declaration). 실제 정의는
 * lib/vfio_user/host/vfio_user.c 내부 (소켓 fd, BAR 정보 배열, IRQ 정보, DMA
 * 매핑 리스트 등)에 캡슐화되어 있고, 외부에는 불투명 포인터로만 노출한다.
 * 호출자는 setup()/release() 사이의 핸들로만 다루며 내부 필드에 직접 접근하지
 * 않는다. SPDK 전반에서 흔한 "opaque handle" 패턴. */

/*
 * [한국어]
 * spdk_vfio_user_pci_bar_access - 지정한 PCI BAR 영역에 대해 R/W I/O 수행
 *
 * @dev:      spdk_vfio_user_setup() 으로 얻은 vfio-user 디바이스 핸들. 내부적으로
 *            서버와 연결된 Unix 도메인 소켓 fd 와 BAR 메타데이터를 보관한다.
 * @index:    접근할 BAR 인덱스 (0~5). NVMe SSD 의 경우 BAR0 가 컨트롤러 MMIO
 *            레지스터(CC, CSTS, AQA, ASQ, ACQ, SQ/CQ 도어벨 …) 영역에 해당.
 * @offset:   해당 BAR 의 시작에서의 바이트 오프셋. NVMe 도어벨이라면
 *            0x1000 + (2*y + (cq?1:0)) * (4 << CAP.DSTRD) 같은 값.
 * @len:      I/O 길이(바이트). 1/2/4/8 등 정렬된 워드 단위.
 * @buf:      is_write=true 면 송신할 데이터 버퍼, false 면 수신 버퍼.
 * @is_write: true=쓰기 (REGION_WRITE 메시지), false=읽기 (REGION_READ 메시지).
 * @return:   0=성공, 음수 errno=실패 (소켓 오류, server 에러 응답 등).
 *
 * 이 함수가 필요한 이유: 일반 VFIO(커널) 에서는 mmap 된 BAR 메모리에 직접
 * MMIO load/store 를 발사하지만, vfio-user 에서는 BAR 가 server 프로세스에 있어
 * 메시지로 R/W 를 대리해야 한다. 본 함수는 그 R/W 를 1회 호출에 추상화한다.
 *
 * 동작 단계:
 *   1) 인자 검증 후 vfio_user_region_access 페이로드 작성(offset/region/count).
 *   2) is_write 면 REGION_WRITE 메시지(헤더 cmd=10) + buf 데이터 송신,
 *      is_read 면 REGION_READ 메시지(cmd=9) 송신 후 응답에서 데이터 수신.
 *   3) 응답 헤더의 flags.error / error_no 를 검사하여 호출자에게 errno 변환.
 *
 * 실행 컨텍스트: NVMe 컨트롤러 초기화/도어벨 발사 경로 — 일반적으로 SPDK
 * 잡 스레드(reactor) 위. 동일한 vfio_device 핸들에 대해 동시 호출 시
 * 메시지 ID 충돌을 막기 위해 lib/vfio_user/host 내부에서 직렬화된다.
 *
 * 호출 체인:
 *   nvme_vfio_user_ctrlr_set_reg_4/8() → [본 함수] → host/vfio_user.c::send_message()
 */
int spdk_vfio_user_pci_bar_access(struct vfio_device *dev, uint32_t index,
				  uint64_t offset, size_t len, void *buf,
				  bool is_write);

/*
 * [한국어]
 * spdk_vfio_user_get_bar_addr - 지정 BAR 가 mmap 가능하면 host 가상주소 반환
 *
 * @dev:    setup() 에서 얻은 vfio-user 디바이스 핸들.
 * @index:  BAR 인덱스 (0~5).
 * @offset: BAR 내부 오프셋 — 페이지 정렬되거나 server 가 mmap 가능한 sub-region.
 * @len:    매핑할 길이.
 * @return: 매핑된 가상주소 포인터, 실패 시 NULL. 반환 포인터는 process VA 로,
 *          load/store 가 그대로 BAR I/O 에 해당 (server 측에서 SHM/fd 공유).
 *
 * 이 함수가 필요한 이유: vfio-user 프로토콜은 일부 region 에 대해
 * VFIO_USER_DEVICE_GET_REGION_IO_FDS 로 fd 와 sparse mmap 정보를 교환할 수
 * 있게 정의되어 있다. 그러면 client 는 그 fd 를 mmap 해서 BAR 를 zero-copy
 * 로 직접 접근 가능하고, 매번 메시지를 주고받는 spdk_vfio_user_pci_bar_access()
 * 보다 훨씬 빠르다. NVMe 도어벨이 hot path 인데, 가능하면 이 경로를 사용.
 *
 * 호출자는 NULL 반환 시 자동으로 메시지 기반 BAR access 로 폴백한다.
 *
 * 호출 체인:
 *   nvme_vfio_user_ctrlr_construct() → [본 함수] → 매핑된 BAR0 주소를
 *   spdk_nvme_ctrlr 의 regs 포인터로 저장 → 이후 doorbell/CC 직접 access.
 */
void *spdk_vfio_user_get_bar_addr(struct vfio_device *dev, uint32_t index,
				  uint64_t offset, uint32_t len);

/*
 * [한국어]
 * spdk_vfio_user_setup - vfio-user 디바이스 부착(connect/handshake/probe) 진입점
 *
 * @path:   server 가 listen 중인 Unix 도메인 소켓 절대경로 (예:
 *          "/var/run/cntrl"). nvme_vfio_user 트랜스포트 ID 로 전달된 traddr.
 * @return: 성공 시 새로 할당된 struct vfio_device * (불투명 핸들), 실패 시 NULL.
 *
 * 이 함수가 필요한 이유: vfio-user client 는 단순한 socket connect 가 아니라
 * 다음 단계의 핸드셰이크/프로빙을 모두 마쳐야 디바이스를 다룰 수 있다.
 *   1) socket(AF_UNIX, SOCK_STREAM) + connect(path).
 *   2) VFIO_USER_VERSION 메시지 교환 (major/minor + JSON capabilities).
 *   3) VFIO_USER_DEVICE_GET_INFO 로 num_regions / num_irqs / flags 획득.
 *   4) 각 region(BAR/PCI config)에 대해 VFIO_USER_DEVICE_GET_REGION_INFO 로
 *      size, flags, mmap 가능성, sparse mmap 영역, fd 를 받아둔다.
 *   5) IRQ 정보(VFIO_USER_DEVICE_GET_IRQ_INFO)를 수집.
 *   6) DMA 영역(SPDK env_dpdk 의 hugepage memseg)을 VFIO_USER_DMA_MAP 으로 등록.
 * 위 절차는 커널 VFIO 의 ioctl 시퀀스를 메시지로 옮긴 것과 1:1 대응한다.
 *
 * 실행 컨텍스트: spdk_nvme_probe/attach 내부 — 보통 RPC 디스패치 또는 앱 init
 * 스레드. blocking 소켓 I/O 를 수반하므로 polled-mode hot path 에서 호출하지 않는다.
 *
 * 호출 체인:
 *   spdk_nvme_attach() → nvme_vfio_user_ctrlr_construct() → [본 함수].
 */
struct vfio_device *spdk_vfio_user_setup(const char *path);

/*
 * [한국어]
 * spdk_vfio_user_release - vfio-user 디바이스 핸들 정리
 *
 * @dev: setup() 으로 얻은 핸들. NULL 허용 여부는 구현체 따름(보통 무시).
 *
 * 동작: 등록된 DMA 매핑들을 VFIO_USER_DMA_UNMAP 으로 해제 → 소켓 close
 *       → 내부 자료구조(BAR fd, region 메타) free → 핸들 free.
 *
 * 실행 컨텍스트: spdk_nvme_detach() 또는 종료 경로. setup() 과 마찬가지로
 * blocking I/O 가 일어나므로 hot path 에서 호출되지 않는다.
 *
 * 호출 체인:
 *   spdk_nvme_detach() → nvme_vfio_user_ctrlr_destruct() → [본 함수].
 */
void spdk_vfio_user_release(struct vfio_device *dev);

/* For fuzzing only */
/*
 * [한국어]
 * spdk_vfio_user_dev_send_request - 임의 vfio_user_command 메시지를 raw 로 발사 (퍼징 전용)
 *
 * @dev:     setup() 으로 얻은 핸들.
 * @command: enum vfio_user_command 값 — 위 spec 헤더에 정의된 1~14 명령.
 * @arg:     명령에 따라 다르게 해석되는 페이로드 버퍼 (예: DMA_MAP 이면
 *           struct vfio_user_dma_map *).
 * @arg_len: arg 바이트 길이.
 * @buf_len: 응답에서 기대하는 추가 데이터 바이트 길이 (REGION_READ 의
 *           읽기 결과처럼 응답에 가변 길이 데이터가 포함되는 경우).
 * @fds:     SCM_RIGHTS 로 함께 전달할 fd 배열 (DMA_MAP 의 hugepage memfd 등).
 * @max_fds: fds 배열의 최대 원소 수 — 응답에서 받은 fd 수의 상한이기도 함.
 * @return:  0 성공, 음수 errno 실패.
 *
 * 이 함수가 필요한 이유: vfio-user 프로토콜의 견고성을 검증하기 위한 fuzzer
 * (test/nvme/vfio_user_fuzz/) 가 정상 경로 검증을 우회해 비정형 메시지를
 * 보내야 하기 때문에 별도로 노출된 raw 진입점이다. 일반 SPDK 코드에서는
 * 절대로 직접 사용하지 않으며, 이 점을 강조하기 위해 위에 "For fuzzing only"
 * 주석이 명시되어 있다.
 *
 * 실행 컨텍스트: 퍼저 main 스레드. 동기 I/O.
 *
 * 호출 체인:
 *   vfio_user_fuzz main() → [본 함수] → host/vfio_user.c 내부 송수신 루틴.
 */
int spdk_vfio_user_dev_send_request(struct vfio_device *dev, enum vfio_user_command command,
				    void *arg, size_t arg_len, size_t buf_len, int *fds,
				    int max_fds);

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 종료. */
#endif

#endif
/* [한국어] _SPDK_VFIO_USER_PCI_H 헤더 가드 종료 — 다중 인클루드 시 재정의 방지. */
