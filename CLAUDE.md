# CLAUDE.md — spdk-study 프로젝트 지침

> 주석 작성 방법론(파일/함수/인라인/구조체 포맷, 핵심 원칙, 공통 작업 원칙, 커밋 규칙)은
> 상위 디렉토리의 [`../CLAUDE.md`](../CLAUDE.md)에서 공통으로 관리한다.
> 이 파일은 **SPDK 도메인에 특화된 지식과 작업 현황**만 정의한다.

## 프로젝트 개요

이 저장소는 [SPDK (Storage Performance Development Kit)](https://spdk.io) 소스 코드를 분석하고 학습하기 위한 스터디 프로젝트이다. SPDK는 커널을 우회하여 유저스페이스에서 NVMe/블록 디바이스를 polled-mode로 구동하는 고성능 스토리지 프레임워크이다.

- **원본**: SPDK (Intel/Linux Foundation)
- **원전 특징**: 유저스페이스 NVMe 드라이버, polled-mode, lockless, DPDK 기반 메모리·스레딩
- **브랜치**: master

## SPDK 핵심 아키텍처

### 전체 I/O 스택

```
[Application]
  ↓ spdk_bdev_read/write (bdev API)
[bdev layer (lib/bdev)]             # 공통 블록 디바이스 추상화
  ↓
[bdev module (bdev_nvme / bdev_aio / bdev_malloc …)]
  ↓
[NVMe driver (lib/nvme)]            # 유저스페이스 NVMe 드라이버
  ↓ MMIO doorbell / PRP / SGL
[PCIe → NVMe SSD Hardware]
```

### 실행/스레드 모델 (Reactor / Poller)

```
spdk_env_init() [DPDK EAL 기반 환경 초기화]
  → spdk_app_start() / spdk_thread_lib_init()
  → 각 CPU 코어에 spdk_reactor 배치 (1 코어 = 1 reactor = 1 user thread)
      → spdk_poller 등록/실행 (무한 polling 루프)
      → spdk_thread_send_msg() (lockless 메시지 전달)
      → SPDK I/O 요청은 해당 스레드(코어)에 고정(affinity)되어 실행
```

### 주요 서브시스템

```
include/spdk/
  ├── env.h         - DPDK 추상화 (hugepage, DMA memory, PCI)
  ├── thread.h      - SPDK thread / poller / message
  ├── bdev.h        - 블록 디바이스 API
  ├── nvme.h        - 유저스페이스 NVMe 드라이버 API
  ├── nvmf.h        - NVMe-over-Fabrics target
  ├── blob.h        - Blobstore (flat object store)
  ├── sock.h        - POSIX/uring/VPP 소켓 추상화
  ├── vhost.h       - vhost-user target
  └── rpc.h         - JSON-RPC 서버

lib/
  ├── env_dpdk/     - DPDK 바인딩
  ├── nvme/         - NVMe 드라이버 구현 (PCIe/Fabrics 공통)
  ├── bdev/         - bdev 코어
  ├── thread/       - reactor/poller 구현
  ├── nvmf/         - NVMe-oF target
  └── ...

module/bdev/        - bdev 백엔드 모듈 (nvme, aio, malloc, raid, lvol …)
```

## SPDK 특화 주석 요구사항

SPDK 코드의 특성상, 주석에 다음 사항을 반드시 포함한다:

1. **유저스페이스 NVMe 경로**: MMIO BAR 매핑, doorbell write, PRP/SGL 생성, CQE 파싱 등 각 단계가 어떤 하드웨어 동작에 해당하는지 명시 (NVMe 1.x 스펙 섹션 참조)
2. **스레드 소유권 (thread affinity)**: 함수가 어느 SPDK thread/reactor에서 호출되어야 하는지, cross-thread 호출 시 `spdk_thread_send_msg`를 쓰는 이유 설명
3. **Polled-mode의 이유**: 인터럽트/문맥교환 회피 목적과 그로 인한 CPU 사용 트레이드오프 설명
4. **Lockless 설계**: 공유 자료구조가 있는 경우, lock-free한 근거(스레드 고정, ring, RCU-like 패턴)를 설명
5. **bdev I/O 경로**: `spdk_bdev_io` 생성 → submit → 모듈 콜백 → completion 경로와 상태 전이 설명
6. **DPDK/EAL 의존성**: hugepage 기반 메모리, `rte_mempool`, `rte_ring` 등이 등장하면 DPDK 개념을 간단히 부연
7. **비동기 완료 콜백 모델**: SPDK 전반의 `cb_fn(ctx, rc, …)` 패턴과 콜백이 실행되는 스레드 컨텍스트 명시
8. **RPC/JSON 스키마**: RPC 핸들러가 있는 경우 JSON 입출력 스키마와 주체(클라이언트/서버) 설명

## 디테일 수준 가이드 (SPDK 구체화)

| 파일 유형 | 상단 블록 | 함수 주석 | 인라인 주석 | 구조체 필드 |
|----------|----------|----------|------------|-----------|
| 공개 헤더 (include/spdk/*.h) | 매우 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| 내부 헤더 (include/spdk_internal/*.h, lib/*/…_internal.h) | 매우 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| NVMe 드라이버 (lib/nvme/*) | 매우 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| bdev 코어 (lib/bdev/*) | 매우 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| NVMe-oF (lib/nvmf/*) | 매우 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| Thread/Reactor (lib/thread/*) | 매우 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| bdev 모듈 (module/bdev/*) | 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| env_dpdk (lib/env_dpdk/*) | 상세 | 모든 함수 | **모든 라인** | 주요 구조체 |
| 유틸리티 (lib/util/*) | 상세 | 모든 함수 | **모든 라인** | 주요 구조체 |
| 앱/예제 (app/, examples/) | 상세 | 모든 함수 | **모든 라인** | 주요 구조체 |
| 테스트 (test/) | 간결 | 주요 함수만 | 특이 케이스만 | - |

## 주요 용어 사전

- **reactor**: CPU 코어에 고정된 SPDK 실행 단위 (1 코어 = 1 reactor)
- **spdk_thread**: reactor에 배치되는 논리 스레드 (poller/메시지를 처리)
- **poller**: 무한 루프에서 주기적으로 호출되는 콜백 (I/O 완료 폴링 등)
- **bdev**: SPDK의 공통 블록 디바이스 추상화
- **bdev_io**: bdev 레이어의 I/O 요청 객체
- **qpair**: NVMe Queue Pair (SQ+CQ 한 쌍)
- **PRP / SGL**: NVMe DMA 기술 (Physical Region Page / Scatter-Gather List)
- **hugepage**: DPDK가 사용하는 2MB/1GB 대형 페이지 (핀된 물리 메모리)
- **mempool / ring**: DPDK의 lockless 메모리 풀 / ring buffer
- **NVMe-oF**: NVMe over Fabrics (RDMA/TCP/FC transport)
- **vhost-user**: QEMU 등과의 vhost 프로토콜 유저스페이스 구현

## 작업 제외 대상

- `dpdk/` 서브모듈 (외부 DPDK 소스) — 주석 작업 대상에서 제외
- `isa-l/`, `isa-l-crypto/`, `ocf/`, `xnvme/` 등 외부 서브모듈
- `build/`, `go/vendor/` 자동 생성/벤더 코드
- `test/` 하위 대용량 자동 테스트 스크립트 — 핵심 유닛 테스트 외 제외

## 주석 작업 진행 현황

### 완료 (14 파일)

- `lib/nvmf/subsystem.c` (4377 → 4748 라인) — 4섹션 상단 블록(subsystem 코어 역할/
  state machine/listener/host/NS 라이프사이클),
  헤더 인클루드/매크로/forward decl 인라인, enum spdk_nvmf_nqn_domain_states §4,
  핵심 §2 함수 헤더 (nvmf_valid_ascii_string / nvmf_nqn_is_valid /
  spdk_nvmf_subsystem_create / spdk_nvmf_subsystem_destroy /
  nvmf_subsystem_set_state / spdk_nvmf_subsystem_start / stop / pause / resume /
  spdk_nvmf_subsystem_add_listener / add_listener_ext / remove_listener /
  spdk_nvmf_subsystem_add_host_ext / remove_host /
  spdk_nvmf_subsystem_add_ns_ext / spdk_nvmf_subsystem_remove_ns /
  spdk_nvmf_subsystem_set_ana_state),
  spdk_nvmf_subsystem_create / nvmf_subsystem_set_state 본문 라인별 인라인.
  잔여(reservation/PR 처리 함수들, ANA helper 일부) 미완.
- `lib/nvmf/tcp.c` (4000 → 4598 라인) — 4섹션 상단 블록(NVMe-TCP wire protocol/
  PDU 흐름/HDgst·DDgst/per-CPU poll group 구조 설명),
  모든 매크로 인라인 주석,
  enum spdk_nvmf_tcp_req_state §4 (전체 16개 상태에 대한 lifecycle 설명),
  enum nvmf_tcp_qpair_state §4,
  핵심 구조체 §4 멀티라인 (spdk_nvmf_tcp_req / spdk_nvmf_tcp_qpair /
  spdk_nvmf_tcp_control_msg / control_msg_list / spdk_nvmf_tcp_poll_group /
  spdk_nvmf_tcp_port / tcp_transport_opts / tcp_psk_entry /
  spdk_nvmf_tcp_transport),
  핵심 §2 함수 헤더 (nvmf_tcp_req_set_state / nvmf_tcp_req_get / nvmf_tcp_create /
  nvmf_tcp_destroy / nvmf_tcp_listen / nvmf_tcp_stop_listen /
  nvmf_tcp_handle_connect / nvmf_tcp_accept / nvmf_tcp_accept_cb /
  nvmf_tcp_capsule_cmd_hdr_handle / capsule_cmd_payload_handle /
  h2c_data_hdr_handle / h2c_data_payload_handle / send_capsule_resp_pdu /
  send_r2t_pdu / _nvmf_tcp_send_c2h_data / nvmf_tcp_sock_process /
  nvmf_tcp_qpair_process / nvmf_tcp_sock_cb / nvmf_tcp_req_complete /
  nvmf_tcp_poll_group_poll / nvmf_tcp_req_process),
  forward declaration 모두 인라인,
  spdk_nvmf_transport_tcp ops 테이블 멤버별 인라인 주석,
  sock_process AWAIT_PDU_READY/CH 케이스 본문 인라인.
  잔여(다수 helper static 함수, qpair_init / mem_resource init / digest 처리 /
  zcopy 처리 / abort 처리 등) 미완.

### 이전 완료 (12 파일)

- `lib/nvmf/ctrlr.c` (5707 → 6312 라인) — 4섹션 상단 블록(파일 역할/위치/연결/주요 함수),
  핵심 매크로(NVMF_CC_RESET_SHN_TIMEOUT_IN_MS, FW_VERSION, NVMF_ABORT_COMMAND_LIMIT 등) 인라인,
  spdk_nvmf_custom_admin_cmd / nvmf_prop §4 멀티라인 구조체 주석,
  핵심 함수(nvmf_ctrlr_create / _nvmf_ctrlr_destruct / nvmf_ctrlr_destruct /
  nvmf_ctrlr_stop_keep_alive_timer / start_keep_alive_timer / stop_association_timer /
  nvmf_qpair_set_ctrlr / nvmf_ctrlr_send_connect_rsp / nvmf_ctrlr_add_qpair /
  nvmf_invalid_connect_response / SPDK_NVMF_INVALID_CONNECT_CMD/DATA 매크로 /
  nvmf_ctrlr_disconnect_qpairs_done / _nvmf_ctrlr_disconnect_qpairs_on_pg /
  nvmf_ctrlr_disconnect_qpairs_on_pg / nvmf_ctrlr_disconnect_io_qpairs_on_pg /
  nvmf_ctrlr_keep_alive_poll / nvmf_ctrlr_cmd_connect / _nvmf_ctrlr_connect /
  nvmf_property_get / nvmf_property_set / nvmf_prop_set_cc / find_prop /
  nvmf_ctrlr_async_event_request / nvmf_ctrlr_get_log_page / nvmf_ctrlr_identify /
  nvmf_ctrlr_process_admin_cmd / nvmf_ctrlr_process_fabrics_cmd /
  nvmf_ctrlr_process_io_cmd / spdk_nvmf_request_complete / spdk_nvmf_request_exec) §2 헤더,
  nvmf_ctrlr_create 본문 핵심 라인 인라인.
- `lib/nvmf/nvmf_rpc.c` (3298 → 3713 라인) — 4섹션 상단 블록,
  주요 ctx 구조체(rpc_get_subsystem / rpc_subsystem_create / rpc_listen_address /
  nvmf_rpc_listen_op / nvmf_rpc_listener_ctx / nvmf_rpc_ns_params / nvmf_rpc_ns_ctx /
  nvmf_rpc_host_ctx / nvmf_rpc_create_transport_ctx) §4 멀티라인 필드 주석,
  핵심 RPC 핸들러(rpc_nvmf_get_subsystems / rpc_nvmf_create_subsystem /
  rpc_nvmf_delete_subsystem / rpc_nvmf_subsystem_add_listener /
  rpc_nvmf_subsystem_remove_listener / rpc_nvmf_subsystem_add_ns /
  rpc_nvmf_subsystem_remove_ns / rpc_nvmf_subsystem_add_host /
  rpc_nvmf_create_target / rpc_nvmf_create_transport / rpc_nvmf_get_stats /
  _rpc_nvmf_subsystem_query) §2 헤더.

### 이전 완료 (10 파일)

- `lib/blob/blobstore.c` (10402 라인) — 4섹션 상단 블록 + 모든 공개 API(spdk_*) §2 함수 헤더
  + 핵심 함수(spdk_bs_init/load/unload/destroy/create_blob/open_blob/close/io_*) 인라인 주석.
  static helper 일부(blob_id_cmp, blob_verify_md_op, bs_claim/release_md_page,
  bs_claim/release_cluster, blob_insert_cluster, bs_allocate_cluster 등)에 §2 주석.
- `lib/ftl/nvc/ftl_nvc_dev.c` (73 라인) — 4섹션 상단 + 모든 함수 §2 + 라인 인라인 + 구조체 §4.
- `lib/ftl/nvc/ftl_nvc_dev.h` (184 라인) — 4섹션 상단 + 모든 ops 멤버 §4 + 매크로 인라인.
- `lib/ftl/nvc/ftl_nvc_bdev_common.c` (94 라인) — 전부 완비.
- `lib/ftl/nvc/ftl_nvc_bdev_common.h` (21 라인) — 전부 완비.
- `lib/ftl/nvc/ftl_nvc_bdev_non_vss.c` (243 라인) — 전부 완비.
- `lib/ftl/nvc/ftl_nvc_bdev_vss.c` (227 라인) — 전부 완비.
- `lib/ftl/upgrade/ftl_band_upgrade.c` (168 라인) — 전부 완비.
- `lib/ftl/upgrade/ftl_chunk_upgrade.c` (149 라인) — 전부 완비.
- `lib/ftl/upgrade/ftl_layout_upgrade.c` (355 라인) — 전부 완비.

### 미완료 (우선순위 순)

| 디렉토리 | 설명 | 우선순위 |
|---------|------|---------|
| include/spdk/ | 공개 API 헤더 (nvme.h, bdev.h, thread.h 등) | P0 |
| lib/nvme/ | 유저스페이스 NVMe 드라이버 | P0 |
| lib/bdev/ | bdev 코어 | P0 |
| lib/thread/ | reactor/poller 구현 | P0 |
| lib/nvmf/ | NVMe-oF target | P1 |
| module/bdev/nvme/ | NVMe bdev 모듈 | P1 |
| module/bdev/aio/, malloc/, raid/, lvol/ | 기타 bdev 모듈 | P2 |
| lib/env_dpdk/ | DPDK 바인딩 | P2 |
| lib/util/, lib/log/, lib/json/ | 유틸리티 | P2 |
| app/, examples/ | 앱/예제 | P3 |

## 빌드 방법

```bash
git submodule update --init
./configure
make -j$(nproc)
```

## 참고

- SPDK 공식 문서: https://spdk.io/doc/
- NVMe 스펙: NVM Express Base Specification 1.x / 2.x
- DPDK 문서: https://doc.dpdk.org/
- SPDK 아키텍처 개요: https://spdk.io/doc/overview.html
