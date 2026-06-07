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

### 완료 (NVMe PCIe 트랜스포트 6 파일 — 2026-05-28 검수 확정)

이 묶음은 lib/nvme 의 **PCIe 트랜스포트 핵심**(BAR 매핑, MMIO doorbell, PRP/SGL 빌드,
CQ phase polling, hotplug/SIGBUS 방어, poll group 다중 트랜스포트 통합, 트랜스포트
vtable 디스패치, 디바이스별 quirk)으로 6개 파일이며, 이번 세션 시점에 모든 파일이
이미 기준에 부합하는 상태(직전 세션들에서 완성)임을 검수했다. 4섹션 상단 블록 + 모든
구조체/enum 필드 §4 멀티라인 + 공개·핵심 static 함수 §2 헤더 + 본문 인라인 주석 모두
충족하며, 코드 변경은 없다.

- `lib/nvme/nvme_pcie_internal.h` (712 라인, 124건) — 4섹션 상단 블록(PCIe 트랜스포트
  내부 구조 + hot-path 헬퍼 + 호출 체인 + shadow doorbell/CMB/PMR/tracker 4KB 고정
  설명), 매크로 인라인(NVME_MIN/MAX_COMPLETIONS, NVME_MAX_SGL_DESCRIPTORS=250,
  NVME_MAX_PRP_LIST_ENTRIES=503, NVME_PCIE_MIN_ADMIN_QUEUE_SIZE=256 — 4KB 경계 보존
  근거), 구조체 §4 멀티라인 (nvme_pcie_ctrlr 전 13필드; nvme_tracker 전 12필드 +
  SPDK_STATIC_ASSERT 3종 인라인 — 4KB 크기/SGL Qword 정렬; nvme_pcie_poll_group
  2필드; nvme_pcie_qpair 전 27필드 hot/cold 분리 설계 + flags 6비트필드 +
  shadow_doorbell 4필드), enum nvme_pcie_qpair_state 4값 §4, hot-path inline
  함수 §2 + 본문 인라인 (need_event NVMe 1.3 §3.1.24 wrap-around unsigned 16-bit
  비교, update_mmio_required wmb/mb 메모리 순서, ring_sq/cq_doorbell fused +
  shadow + TLS g_thread_mmio_ctrlr 마킹), 모든 함수 프로토타입 §2 헤더 (40+개).

- `lib/nvme/nvme_pcie.c` (1939 라인, 282건) — 4섹션 상단 블록(probe/BAR 매핑/MMIO
  레지스터/SIGBUS 방어/CMB·PMR 관리/hotplug 5대 책임), struct nvme_pcie_enum_ctx §4
  (3필드), 전역 g_signal_lock/g_sigset/g_hotplug_filter_cb 인라인, 모든 정적 함수
  §2 헤더 + 본문 라인별 — nvme_sigbus_fault_sighandler(PCIe link loss → BAR
  MAP_FIXED|MAP_ANONYMOUS remap 0xFF 채움 + atomic CAS), set/get_reg_4/8(TLS 마커
  + ~value==0 link-loss 감지), ASQ/ACQ/AQA/CMBLOC/CMBSZ/PMRCAP/PMRCTL/PMRSTS/
  PMRMSCL/PMRMSCU wrapper(NVMe spec §3), ★ map_cmb(CMBSZ unit_size = 2^(12+4*SZU)
  → CMBLOC BIR/OFST → spdk_pci_device_map_bar), PMR 7종, allocate/free_bars,
  pcie_nvme_enum_cb, ctrlr_scan, ctrlr_construct(claim → zmalloc SHARE → BAR
  alloc → PCI Config 0x404 BME+INTX_DISABLE → doorbell_stride → admin_qpair →
  SIGBUS 핸들러 등록), ctrlr_enable(ASQ/ACQ/AQA 0-based), ctrlr_destruct,
  ctrlr_enable_interrupts(VFIO MSI-X), spdk_nvme_pcie_set_hotplug_filter,
  nvme_pci_driver_id 테이블 + SPDK_PCI_DRIVER_REGISTER(NEED_MAPPING + WC_ACTIVATE),
  ★★★ pcie_ops 트랜스포트 vtable 35개 멤버별 인라인, SPDK_NVME_TRANSPORT_REGISTER
  constructor 패턴.

- `lib/nvme/nvme_pcie_common.c` (3313 라인, 595건) — 4섹션 상단 블록(PCIe/vfio-user
  공유 hot-path: SQE 빌드 → SQ[sq_tail] 기록 → doorbell MMIO; CQ phase bit polling
  → CQE 파싱 → tracker → req 콜백), __thread g_thread_mmio_ctrlr/g_dummy_stat 전역
  §4, ★★★ 핵심 함수 §2 + 본문 라인별 — vtophys(PCIe vs vfio-user IOVA=VA),
  qpair_reset(phase 1 + CQ 0 memset NVMe spec §4.6), completion_create_sq/cq_cb
  (WAIT_FOR_CQ → WAIT_FOR_SQ → READY 전이), copy_command(SSE2 _mm_stream_si128
  4× = 64B non-temporal — cache pollution 회피 + WC buffer 활용), copy_command_
  mmio(QEMU 8B×8 quirk 경로), ★★★ submit_tracker(64B SQE copy + sq_tail++ wrap +
  doorbell ring), abort_trackers(last 미리 저장 — cb_fn 재제출 시 무한 루프 방지),
  ★★★ process_completions(phase bit 검사 + prefetch 다음 tracker + PPC/RISC-V/
  aarch64 명시적 memory barrier 분기 + cq_head wrap phase 토글 + CQ doorbell +
  delay_cmd_submit batch flush + admin 후처리 + pending vtophys 실패 정리 8단계),
  ★★★ prp_list_append(PRP1 페이지내 offset 허용 / 2번째 이후 4KB 정렬 강제 /
  3개 이상이면 PRP list 주소 PRP2에), build_*_request 5종, g_nvme_pcie_build_req_
  table 2D 분기 테이블, build_metadata(MPTR=물리주소 vs MPTR_SGL 분기 + meta_sgl
  직전 배치 약속), ★★★★ submit_request(tracker pop → PSDT 기본 PRP → 빌더 테이블
  → metadata → submit_tracker + admin lock + -EAGAIN 큐잉 + admin PRP 강제),
  poll_group 10종, SPDK_TRACE_REGISTER_FN trace point 6종.

- `lib/nvme/nvme_transport.c` (1820 라인, 242건) — 4섹션 상단 블록(트랜스포트 vtable
  디스패치 + multi-process PCIe 안전성 + async fallback + hot/cold 경계 + qpair->
  transport 캐시 vs 매번 lookup 이유), SPDK_MAX_NUM_OF_TRANSPORTS=16, struct
  spdk_nvme_transport §4, 전역 5종 §4, 모든 공개 함수 §2 + 본문 인라인 —
  transport_register(constructor 매크로 + 이중 등록 assert), ctrlr 수명주기 7종,
  ★ 레지스터 sync R/W 4종, ★ async fallback 4종(sync + 가짜 완료 큐잉 + pid 보존),
  CMB 3종, PMR 4종, ★ create_io_qpair(qpair->transport 캐시 + admin 제외 이유),
  delete_io_qpair(multi-process 신뢰 금지), ★★ connect_qpair(CONNECTING 상태머신 +
  busy-wait + poll_group fabric 분기 7단계), disconnect_qpair_done(FABRIC fallback
  + poll_group fd 깨우기), ★★ qpair hot-path 공통 패턴(I/O likely → 캐시 / admin →
  lookup) — abort_reqs/reset/★★★ submit_request/process_completions/iterate_
  requests/authenticate, ★★ poll_group 9종(connected/disconnected STAILQ 이동 불변식
  + num_connected_qpairs + idempotent + -EINPROGRESS → 0 변환), get_trtype,
  ★ get_opts/set_opts(opts_size ABI 호환 SET_FIELD + STATIC_ASSERT),
  ★ spdk_nvme_ctrlr_get_registers(volatile + link-loss 주의).

- `lib/nvme/nvme_poll_group.c` (1248 라인, 191건) — 4섹션 상단 블록(다중 트랜스포트
  통합 poll group + accel 오프로드 + interrupt 모드 epoll + disconnected 통지 4대
  가치 + spdk_thread affinity 락리스 근거), poll_group_create(accel_fn_table ABI
  호환 SET_FIELD + finish/reverse/abort 3종 일관성 XOR 검증 + append+finish 의존성
  + fd_group 생성 5단계 본문 라인별), set_interrupt_callback(-EEXIST 정책),
  Linux 전용 — read/★ write_disconnect_qpair_fd(트랜스포트 epoll 깨우기 위해 8B
  write), add_disconnect_qpair_fd(eventfd EFD_NONBLOCK|EFD_CLOEXEC), 비-Linux stub,
  ★ add(qpair DISCONNECTED 검증 + enable_interrupts 첫 qpair 시 고정 + tgroup 검색
  → dlopen 트랜스포트 lazy-create 5단계), connect/disconnect_qpair(롤백 정합성),
  wait(timeout=-1 무한 + disconnected 우선 통지), ★★★ process_completions(재귀
  가드 in_process_completions + error_reason 누적 + num_completions 합산),
  ★ all_connected(-EIO/EAGAIN/0 3단계 우선순위 break), destroy(tgroup 일관성 복원 +
  fd_group 정리), get_stats/free_stats(transports_count 2단계 측정 + trtype 매칭).

- `lib/nvme/nvme_quirks.c` (392 라인, 60건) — 4섹션 상단 블록(NVMe 벤더별 비표준
  동작 보정 테이블 + PCI ID 매칭 + 호출 시점 attach 1회 + read-only 락 없음 근거 +
  19종 quirk 플래그 의미 요약), struct nvme_quirk §4 (id 5튜플 와일드카드 + flags
  비트마스크), nvme_quirks[] 18행 — 각 행마다 모델 식별 + quirk 비트 의미 인라인
  (Intel P3500/P3520/P4500/P5510/P5610 시리즈 + 0A55/5845/2700/4140, Memblaze,
  Samsung a821/a822/a826, VirtualBox, RedHat, CNEX OCSSD, VMware SHST_COMPLETE,
  Huawei NOT_USE_SGL, Microsoft Azure, Micron MSIX), pci_id_match(5필드 AND +
  와일드카드) §2 + 본문, nvme_get_quirks(sentinel 종료 + 선형 탐색 + PRINT_QUIRK
  매크로로 활성 비트 19종 출력 + early exit) §2 + 본문 라인별.

### 완료 (lib/log·json·jsonrpc·rpc·notify·conf·trace·trace_parser·keyring 23 파일 — 2026-05-28 검수 확정)

SPDK의 관리/관찰(management/observation) 평면을 구성하는 9개 라이브러리 디렉토리의
모든 .c/.h(C++) 파일에 대해 4섹션 상단 블록 + 모든 함수 §2 헤더 + 모든 구조체 필드 §4 멀티라인
+ #include/매크로/forward decl/실행 라인 §3 인라인이 갖춰진 것을 검수했다. 이번 라운드에서는
잔여 갭(json_write.c의 named_int64/uint64/double 함수 §2 헤더, rpc.c의 struct rpc_get_methods
§4 필드 주석)만 보강하고 나머지는 직전 세션들에서 이미 완성된 상태를 검수·등재 기록한다.

대상 파일(총 23개):

- `lib/log/log.c` (855 라인) — spdk_log/vlog 통합 출력단, 두 단계 레벨 필터(syslog/stderr),
  싱크 추상화(g_log_opts), spdk_log_dump hex+ASCII, vsnprintf→vasprintf 폴백 라인별 인라인.
- `lib/log/log_deprecated.c` (429 라인) — spdk_deprecation/SPDK_LOG_DEPRECATED 추적기,
  CLOCK_MONOTONIC epoch + rate-limit deferred 카운팅, race-tolerant lockless 정책 명시.
- `lib/log/log_flags.c` (410 라인) — 컴포넌트별 DEBUG 플래그 TAILQ(strcasecmp 정렬), fnmatch
  glob 매칭 spdk_log_set/clear_flag, spdk_log_usage 100자 폭 줄바꿈 출력기.
- `lib/json/json_parse.c` (873 라인) — RFC 8259 JSON 토크나이저(IN_PLACE NUL-종결, JSON5
  주석 옵션, UTF-8 codepoint 검증, 숫자 grammar exponent/fraction state machine).
- `lib/json/json_util.c` (1014 라인) — JSON value 디코더 패밀리(bool/int*/uint*/string/uuid/
  array/object), spdk_json_decode_object 매핑 테이블 처리, json_num 정밀도 분류.
- `lib/json/json_write.c` (1223 라인) — Streaming JSON writer 4KB buf + emit_buf_full→
  flush_buf→write_cb, RFC 8259 escape (UTF-8→codepoint→\uXXXX surrogate pair),
  named_* 패밀리 (null/bool/uint8/16/32/64/int32/64/double/string/string_fmt/array/object).
- `lib/jsonrpc/jsonrpc_client.c` (376 라인) — JSON-RPC 2.0 클라이언트 코어: 응답 파싱
  (jsonrpc/id/result/error 4개 capture 디코더), 요청 prologue/epilogue 직렬화.
- `lib/jsonrpc/jsonrpc_client_tcp.c` (679 라인) — 클라이언트 TCP transport: connect 비동기
  EAGAIN/EINPROGRESS 폴링, send/recv 파편 처리, response 슬롯 단일점유 모델.
- `lib/jsonrpc/jsonrpc_server.c` (864 라인) — JSON-RPC 2.0 서버 코어: 요청 디코딩, batch
  요청 [..] vs 단건 처리, jsonrpc_server_write_cb 직렬화 sink + send buf capacity-doubling,
  notification(id 없음) 무응답 규칙, JSON-RPC 표준 에러 코드 (-32600~-32603, INVALID_STATE).
- `lib/jsonrpc/jsonrpc_server_tcp.c` (776 라인) — 서버 TCP/Unix-socket transport: AF_UNIX
  listen + accept poller, 연결당 conn 객체 + ring buffer 송신 큐, close_listener 후
  pending 응답 flush 종료 절차.
- `lib/jsonrpc/jsonrpc_internal.h` (530 라인) — 디렉토리 내부 공유 자료구조: spdk_jsonrpc_
  request / server_conn / server / client_request / client_response_internal / client 6개
  구조체 모든 필드 §4 멀티라인 (총 23개 필드 그룹).
- `lib/rpc/rpc.c` (827 라인) — SPDK_RPC_REGISTER 매크로의 constructor-시점 등록 인프라,
  g_rpc_methods SLIST + state mask(STARTUP/RUNTIME) gating, AF_UNIX 서버 라이프사이클,
  custom RPC server 멀티 인스턴스 지원, allowlist 기반 method 필터, rpc_get_methods 응답.
- `lib/notify/notify.c` (416 라인) — 1024-slot 환형 이벤트 버퍼 + 단조 증가 64bit ID 모델,
  spdk_notify_type_register/send/foreach_event/foreach_type, at-most-once 보장 명시.
- `lib/notify/notify_rpc.c` (316 라인) — notify_get_types / notify_get_notifications RPC,
  rpc_notify_get_notifications id/max optional 디코더.
- `lib/conf/conf.c` (1219 라인) — 레거시 INI 파서 (deprecated): 섹션[Name#num] 헤더, key-
  value 다중 값, '\\' line-continuation, '#' 주석, fgets_line 가변 길이 reader, set_as_default
  + CHECK_CP_OR_USE_DEFAULT, merge_sections 토글, 4계층 트리(conf → section → item → value).
- `lib/trace/trace.c` (808 라인) — /dev/shm 위 trace 공유메모리 + per-lcore lockless circular
  buffer, _spdk_trace_record() entry chaining(8B 초과 가변 인자), spdk_trace_register_user_
  thread bit_array 슬롯, mlock + ftruncate + mmap(MAP_SHARED), tsc_rate 캡처.
- `lib/trace/trace_flags.c` (1133 라인) — tpoint owner/object/group 메타데이터 등록,
  spdk_trace_register_owner_type/_object/_description_ext API, tpoint_mask 토글, owner ring
  buffer 1024 슬롯 spinlock 보호, JSON dump 헬퍼(trace_get_*).
- `lib/trace/trace_rpc.c` (493 라인) — trace_get_info / trace_get_tpoint_group_mask /
  trace_set_tpoint_mask / trace_clear_tpoint_mask / trace_enable_tpoint_group /
  trace_disable_tpoint_group RPC.
- `lib/trace/trace_internal.h` (97 라인) — lib/trace/ 내부 공유 (trace_get_shm_name,
  trace_flags_init/fini) — trace_rpc.c↔trace.c↔trace_flags.c 협력 인터페이스.
- `lib/keyring/keyring.c` (855 라인) — NVMe-oF TLS PSK / DH-CHAP secret / AES-XTS DEK
  통합 keyring 본체: 모듈(file/Linux kernel keyring) 등록, name 기반 lookup TAILQ,
  refcnt lifecycle, "removed but referenced" 보존(removed_keys TAILQ), probe_key 자동 import.
- `lib/keyring/keyring_rpc.c` (151 라인) — keyring_get_keys RPC, SPDK_KEYRING_FOR_EACH_ALL
  플래그로 active+removed 키 모두 노출 (refcnt 디버깅용).
- `lib/keyring/keyring_internal.h` (70 라인) — keyring_dump_key_info 내부 헬퍼 선언
  (keyring.c 정의, keyring_rpc.c 호출).
- `lib/trace_parser/trace.cpp` (810 라인) — trace 바이너리 후처리: /dev/shm 또는 파일을
  mmap(PROT_READ) → spdk_trace_history 환형 버퍼 시간순 정렬(std::map) → buffer continuation
  패치 → spdk_trace_parser_entry 스트리밍 반환, object 단위 related 추적.

본 라운드의 갭 보강 편집:
- `lib/json/json_write.c`: spdk_json_write_named_int64 / _named_uint64 / _named_double 세
  공개 API에 §2 단축 헤더(한 줄 의도/사용처) 추가. 기존엔 패밀리 공통 블록 주석만 있었음.
- `lib/rpc/rpc.c`: struct rpc_get_methods의 current / include_aliases 두 필드에 §4 멀티라인
  주석 추가(설정자/읽는 자/값 범위/동기화).

### 완료 (공개 헤더 P0 9 파일 — 2026-05-28 검수 확정)

`include/spdk/` 하위 P0 우선순위 공개 헤더(요청된 9개 — `env.h`, `thread.h`, `bdev.h`,
`nvme.h`, `nvme_spec.h`, `bdev_module.h`, `bdev_zone.h`, `event.h`, `env_dpdk.h`)에 대해
4섹션 상단 블록 + 모든 공개 API §2 헤더 + 모든 구조체/enum 필드 §4 멀티라인 +
#include/매크로/forward decl/typedef 인라인이 갖춰진 것을 검수했다.

검수 시점에 9개 파일 모두 이미 기준을 충족하는 상태였으며, 각 파일의 주요 커버리지는
다음과 같다:

- `env.h` (3445 라인, 237 한국어 주석): DPDK 추상화 — hugepage DMA·NUMA-aware mempool/ring·
  PCI enumerate/attach·vtophys·MSI-X efd. spdk_env_opts (128B STATIC_ASSERT) 모든 필드 §4
  (name/core_mask/lcore_map/shm_id/mem_channel/main_core/mem_size/no_pci/hugepage_*/
  iova_mode/base_virtaddr/env_context/vf_token/opts_size/enforce_numa/reserved 패딩),
  spdk_pci_device 전체 vtable + internal §4, spdk_pci_device_provider §4,
  spdk_mem_map_ops §4, enum spdk_ring_type/mem_map_notify_action/pci_event_type §4,
  공개 함수 100+ §2 (malloc/zmalloc/realloc/free + dma_변형, memzone 5종, mempool 11종,
  env_get_*_core, ring, vtophys, pci_driver_register, pci_enumerate, pci_device 32종,
  pci_addr 3종, mem_map 5종, mem_register/unregister, pci_event_listen 등).

- `thread.h` (2860 라인, 191 한국어 주석): SPDK 스레드/폴러/I/O 채널/메시지 패싱 +
  lockless 5개 영역. enum spdk_thread_poller_rc/spdk_thread_op §4,
  forward decl spdk_thread/poller/io_channel_iter 상세,
  typedef spdk_new_thread_fn/op_fn/msg_fn/poller_fn/channel_msg/channel_for_each_cpl/
  io_channel_create/destroy_cb/iobuf_get_cb/post_poller_fn 모두 §4,
  spdk_thread_stats/poller_stats/io_channel/iobuf_opts/pool_cache/buffer/entry/channel/
  module_stats/spinlock 모든 필드 §4,
  공개 함수 80+ §2 (thread_lib_init/_ext/_fini, thread_create/destroy/exit/poll/send_msg,
  poller_register/_named/_unregister/_pause, io_device_register/unregister,
  get/put_io_channel, for_each_channel/_continue, iobuf_get/put/initialize,
  spinlock_init/lock, interrupt_register).

- `bdev.h` (4214 라인, 290 한국어 주석): bdev 사용자 API — open/io_channel/read/write/
  unmap/flush/reset/zone/ext_io_opts/histogram/QoS/passthru. enum spdk_bdev_io_type 23종/
  qos_rate_limit_type/event_type §4, struct spdk_bdev_opts/io_stat/
  enable_histogram_opts/ext_io_opts/open_opts/io_wait_entry §4 모든 필드,
  I/O 발행 함수 40+ §2 (read/readv/_blocks/_with_md/_ext, write 12종, write_zeroes,
  unmap/flush/reset/nvme_nssr/abort, compare/comparev_and_writev, zcopy_start/end,
  copy_blocks, seek_data/hole, nvme_admin/io_passthru, histogram, for_each_channel).

- `nvme.h` (6964 라인, 495 한국어 주석): 유저스페이스 NVMe 드라이버 9개 영역 모두 커버 —
  transport_id, probe/attach 라이프사이클, admin 명령, reset/disconnect, namespace 질의,
  qpair, NVM 명령, poll_group, NVMe-oF discovery. spdk_nvme_ctrlr_opts (856B opts_size ABI)
  모든 필드 §4 (num_io_queues/hostnqn/keep_alive/transport_*/dhchap_*/tls_psk),
  spdk_nvme_io_qpair_opts/transport_id/host_id/path_id/ns_cmd_ext_io_opts/transport_opts §4,
  enum spdk_nvme_transport_type/qprio/qp_failure_reason §4,
  공개 함수 150+ §2 (probe/connect/detach, ctrlr_reset/disconnect, alloc_io_qpair,
  qpair_process_completions, ns_cmd_read/write/compare/copy/dataset_management 모든 변형,
  poll_group_create/process_completions, ctrlr_cmd_admin_raw/get_log_page/set_features/
  format/security_send/firmware_*, authenticate, cuse_register).

- `nvme_spec.h` (6801 라인, 1289 한국어 주석): NVMe Base 1.x/2.x + NVMe-oF + ZNS + FDP 와이어
  포맷 단일 소스. 컨트롤러 레지스터 union(cap/vs/cc/csts/aqa/asq/acq/cmbsz/pmrcap/bpmbl) 모든
  비트필드 §4, 모든 opcode/CNS/LID/FID/AER 이벤트 enum §4, spdk_nvme_cmd/cpl/sgl_descriptor §4,
  spdk_nvme_identify_ctrlr_data/identify_ns_data/identify_zns_ns_data/psd/smart_log/
  error_information_entry/fw_slot_information_log/ana_log_page/sanitize_status_log/
  endurance_group_log/predictable_lat_log/fdp_*_log 모든 필드 §4,
  spdk_nvme_feat_* 27종 union §4, format/security_send/firmware_commit/zns_zone_descriptor/
  fabrics_command/connect_data/auth_send/auth_receive 모든 필드 §4.

- `bdev_module.h` (3425 라인, 394 한국어 주석): bdev 모듈 작성자 인터페이스 — 모듈 등록·
  fn_table·bdev_io 객체·claim API·examine·set_status helper. struct spdk_bdev_module 모든
  필드 §4 (name/module_init/fini/get_ctx_size/examine_config/examine_disk/config_json/
  async_init/async_fini/internal), spdk_bdev_fn_table 모든 ops 멤버 §4 (destruct/
  submit_request/io_type_supported/get_io_channel/dump_info_json 등), enum
  spdk_bdev_io_status/claim_type §4, struct spdk_bdev (name/aliases/blocklen/blockcnt/
  required_alignment/max_segment_size/uuid/zoned/dif_type/zone_size 등 모든 필드) §4,
  ★ struct spdk_bdev_io ★ 모든 필드 §4 (bdev/internal/ch/type/status/iov/u.bdev/u.reset/
  u.abort/u.nvme_passthru/u.zone_mgmt), 공개 함수 §2 (register/unregister, examine_done,
  io_complete/_nvme_status/_scsi_status, claim_bdev_v1/v2, queue_io_wait_done, quiesce,
  part_base_construct, SPDK_BDEV_MODULE_REGISTER 매크로).

- `bdev_zone.h` (486 라인, 45 한국어 주석): NVMe ZNS / ZAC/ZBC zoned 스토리지 공개 API.
  enum spdk_bdev_zone_type/action/state §4 (zone 상태머신 EMPTY→IMP_OPEN→FULL 명시),
  struct spdk_bdev_zone_info §4 (zone_id/write_pointer/capacity/state/type), 공개 함수 §2
  (get_zone_size/_num_zones/_max_open/_active_zones, get_zone_info, zone_management,
  zone_append/appendv/_with_md, io_get_append_location).

- `event.h` (1079 라인, 95 한국어 주석): SPDK 이벤트 프레임워크 — spdk_app_opts/start/stop,
  cross-core spdk_event_call. spdk_event_call vs spdk_thread_send_msg 차이 (lcore vs
  spdk_thread 디스패치 단위 + 인자 1 vs 2) 명시. struct spdk_app_opts (packed 253B,
  STATIC_ASSERT) 모든 필드 §4 ABI reserved 패딩 포함, enum spdk_app_parse_args_rvals §4,
  SPDK_APP_GETOPT_STRING 매크로 인라인, 공개 함수 §2 (app_opts_init/_start/_fini/
  _start_shutdown/_stop/_get_shm_id/_parse_core_mask/_get_core_mask/_parse_args/_usage/
  _setup_trace, event_allocate/_call, framework_enable_context_switch_monitor).

- `env_dpdk.h` (418 라인, 18 한국어 주석): DPDK 직접 초기화 경로 — 외부 DPDK 앱 임베드
  post-init/post-fini 패턴, rte_eal_cleanup 권한 분리, legacy_mem 인자 의미와 vfio DMA 매핑
  전략 분기, spdk_env_init vs spdk_env_dpdk_post_init 배타적 진입점. struct
  spdk_env_dpdk_mem_stats §4 모든 필드 (heap_totalsz/freesz/greatest_free_size/heap_allocsz/
  free_count/alloc_count — 단편화 지표 의미 + 스냅샷 동기화 한계 명시), 공개 함수 §2
  (post_init(legacy_mem)/post_fini/external_init/dump_mem_stats/get_mem_stats — EAL lifecycle
  소유권 분리, 호출 체인 상세).

### 완료 (공개 헤더 P1 13 파일 — 2026-05-28 검수 확정)

`include/spdk/` 하위 P1 우선순위 공개 헤더(요청된 13개 — `sock.h`, `blob.h`, `blob_bdev.h`,
`nvmf.h`, `nvmf_spec.h`, `rpc.h`, `init.h`, `jsonrpc.h`, `json.h`, `nvmf_cmd.h`,
`nvmf_transport.h`, `vhost.h`, `nvmf_fc_spec.h`)에 대해 4섹션 상단 블록 + 모든 공개 API
§2 헤더 + 모든 구조체/enum 필드 §4 멀티라인 + #include/매크로/forward decl/typedef
§3 인라인이 갖춰진 것을 검수했다. iscsi/scsi 관련 헤더는 작업 대상이 아니므로 제외했다.

이번 세션 시작 시점에 13개 파일 모두 이미 기준을 충족하는 상태였으며, 다음을 확인했다:

- 13개 파일 모두 상단에 `=== 파일의 역할 ===` / `=== 전체 아키텍처에서의 위치 ===` /
  `=== 타 모듈과의 연결 ===` / `=== 주요 함수/구조체 요약 ===` 4섹션 모두 존재.
- `sock.h` (1824 라인, 111 한국어 주석): SPDK 소켓 추상화(posix/uring/ssl/vpp vtable),
  NVMe-oF TCP / iSCSI / vhost / RPC 공유 트랜스포트, reactor thread affinity, writev_async/
  sock_group/zerocopy/TLS PSK 통합 모델. 모든 공개 API(40+ 함수: initialize/connect[_ext|
  _async]/listen[_ext]/accept/close/flush/recv/writev[_async]/readv/recv_next/group_create/
  add_sock/poll/impl_get_opts/set_default_impl 등) §2 헤더, spdk_sock_request/opts/
  impl_opts/initialize_opts §4 멀티라인 필드 주석.
- `blob.h` (2819 라인, 178 한국어 주석): Blobstore flat object store, cluster/page/blob ID
  모델, blob 라이프사이클(create→open→io→close→delete), snapshot/clone/inflate/decouple,
  xattr, bs_dev vtable 인터페이스. 모든 공개 API §2, enum bs_clear_method/bs_dev_type/
  blob_op_type/blob_open_flags §4, struct spdk_bs_dev/bs_opts/blob_opts/blob_md_page §4.
- `blob_bdev.h` (290 라인, 17 한국어 주석): Blobstore↔bdev 어댑터, spdk_bs_dev vtable의
  bdev I/O 구현. struct spdk_bdev_bs_dev_opts §4 (ABI opts_size 패턴), 4개 공개 API §2
  (spdk_bdev_create_bs_dev[_ext], spdk_bdev_update_bs_blockcnt, spdk_bs_bdev_claim).
- `nvmf.h` (2901 라인, 253 한국어 주석): NVMe-oF target 1차 공개 API, 10대 영역(tgt
  lifecycle/subsystem CRUD/ns 추가/listener·host·referral/ANA multipath/Reservation/
  DH-HMAC-CHAP+TLS PSK/poll group/transport/write_config_json), 객체 그래프 다이어그램,
  11개 도메인 개념 명시. 모든 spdk_nvmf_* 공개 API 100+ §2 헤더, 모든 opts/event/policy
  구조체 §4, enum spdk_nvmf_tgt_discovery_filter §4.
- `nvmf_spec.h` (1634 라인, 375 한국어 주석): NVMe-oF 1.x/2.0 와이어 포맷 스펙 미러,
  capsule/property/connect/discovery/auth/RDMA private/TCP PDU/DH-CHAP 메시지 전부.
  #pragma pack(1) ABI 강제, 모든 와이어 구조체 §4 (offset/사이즈 명시), 모든 enum
  (fabric_cmd_types/status_code/rdma_qptype/prtype/cms/trtype/adrfam/subtype/auth_*/
  dhchap_hash/dhgroup/auth_failure_reason/tcp_pdu_type/tcp_term_req_fes 등) §4,
  모든 SPDK_STATIC_ASSERT 사이즈/offset 검증 인라인.
- `rpc.h` (363 라인, 25 한국어 주석): SPDK JSON-RPC 메서드 등록/디스패치, SPDK_RPC_REGISTER
  constructor 자동 등록, STARTUP/RUNTIME state mask, deprecated alias, allowlist 보안,
  server lifecycle. SPDK_RPC_REGISTER/REGISTER_ALIAS_DEPRECATED 매크로 인라인,
  RPC state bit §4, typedef spdk_rpc_method_handler §4, 모든 공개 API §2 (register_method/
  register_alias/verify_methods/server_listen/accept/close/set_state/get_state/
  set_allowlist 등).
- `init.h` (580 라인, 28 한국어 주석): subsystem init/fini 위상정렬, SPDK_DEFAULT_RPC_ADDR,
  JSON-RPC server lifecycle(initialize/finish/pause/resume), STARTUP→RUNTIME phase 전환,
  spdk_subsystem_load_config의 RPC replay 모델. SPDK_DEFAULT_RPC_ADDR 매크로 인라인,
  struct spdk_rpc_opts §4 (size/log_file/log_level), typedef spdk_subsystem_init_fn/fini_fn §4,
  모든 공개 API §2 (rpc_initialize/finish/server_finish/server_pause/resume,
  subsystem_init/fini/load_config/write_config_json).
- `jsonrpc.h` (715 라인, 53 한국어 주석): JSON-RPC 2.0 server/client 추상화, server(listen/
  poll/handle_request_fn)와 client(connect/send_request/poll/get_response) 양측 API,
  begin_result/end_result 응답 작성 패턴, RPC 트레이스 로그. 에러 코드 매크로
  (PARSE_ERROR/INVALID_REQUEST/METHOD_NOT_FOUND/INVALID_PARAMS/INTERNAL_ERROR/
  INVALID_STATE) §4, typedef spdk_jsonrpc_handle_request_fn/conn_closed_fn §4, 모든 공개 API §2.
- `json.h` (843 라인, 116 한국어 주석): zero-copy JSON 파서 + streaming writer,
  spdk_json_val 인덱싱 모델, decode helper 패밀리, write_ctx 콤마/콜론 상태머신, RFC 8259
  호환. enum spdk_json_val_type §4 (11종 비트 플래그), 파싱 플래그(DECODE_IN_PLACE) §4,
  struct spdk_json_val/object_decoder §4, typedef spdk_json_decode_fn/write_cb §4, 모든
  공개 API §2 (json_parse/decode_object/array/bool/uintN/intN/string/uuid/number_to_*/
  val_len/strequal/strdup/find/object_first/array_first/next/write_*/write_named_* 50+).
- `nvmf_cmd.h` (729 라인, 33 한국어 주석): NVMe-oF target custom admin command hook API,
  opcode별 사용자 핸들러 등록, passthru 모드, custom 핸들러 보조 함수(ctrlr/subsystem/cmd/
  cpl/iov 추출, bdev I/O 위임, identify 페이지 부분 채움, abort). enum
  spdk_nvmf_request_exec_status §4, typedef spdk_nvmf_custom_cmd_hdlr/passthru_cmd_cb §4,
  모든 공개 API §2 (set_custom_admin_cmd_hdlr/set_passthru_admin_cmd/bdev_ctrlr_nvme_passthru/
  ctrlr_identify_*/request_get_*/request_copy_to/from_buf 등).
- `nvmf_transport.h` (1718 라인, 276 한국어 주석): NVMe-oF Target transport 플러그인 vtable,
  SPDK_NVMF_TRANSPORT_REGISTER constructor 등록, transport↔core 경계, poll group fan-in,
  qpair affinity, zero-copy phase machine, controller migration. enum
  spdk_nvmf_zcopy_phase/qpair_state §4, 핵심 구조체 §4 멀티라인 모두 (spdk_nvmf_request의
  50+ 필드, spdk_nvmf_qpair/transport_poll_group/poll_group/listener/ctrlr_data/transport/
  transport_ops 30+ 콜백 멤버, registers/ctrlr_feat/ctrlr_migr_data), typedef
  spdk_nvmf_state_change_done/transport_qpair_fini_cb §4, 공개 API §2.
- `vhost.h` (814 라인, 38 한국어 주석): SPDK vhost-user target, vhost-user 프로토콜
  master(QEMU)↔slave(SPDK) 협상, virtqueue/eventfd/memory region, vhost-blk/vhost-scsi 두
  vdev 타입, AF_UNIX socket 경로, session별 spdk_thread affinity, 전역 spdk_vhost_lock vs
  lockless I/O fast path. typedef spdk_vhost_init_cb/fini_cb §4, 모든 공개 API §2
  (vhost_set_socket_path/blk_init/fini/config_json/scsi_init/fini/config_json/dev_find/next/
  get_name/get_cpumask/lock/trylock/unlock/blk_construct/scsi_dev_construct[_no_start]/
  add_tgt/get_tgt/remove_tgt/dev_remove/shutdown_cb).
- `nvmf_fc_spec.h` (866 라인, 212 한국어 주석): NVMe over Fibre Channel 와이어 스펙 미러
  (INCITS T11 FC-NVMe), 프레임 헤더/IU/Link Service/디스크립터/거절 사유 코드, HBA 드라이버↔
  SPDK FC poller 간 빅엔디언 ABI. 모든 매크로(FCNVME_R_CTL_*/F_CTL_*/TYPE_*/LS_*/LSDESC_*/
  RJT_RC_*/RJT_EXP_*) §4, 모든 구조체 §4 (frame_hdr/cmnd_iu/ersp_iu/xfer_rdy_iu/
  ls_cr_assoc_*/ls_cr_conn_*/ls_disconnect_*/ls_rjt/lsdesc_*), spdk_nvmf_fc_wwn union §4
  (wwn 64-bit / octets 8-byte 배열).

작업 대상 외 공개 헤더(iscsi.h, scsi.h 등)는 사용자가 명시적으로 제외했다.

### 완료 (lib/util 26 파일 — 2026-05-28 검수 확정)

`lib/util/` 하위의 모든 .c/.h 파일(요청된 26개 — `pipe.c`, `file.c`, `md5.c`,
`crc32_ieee.c`, `crc32.c`, `crc32c.c`, `crc16.c`, `crc64.c`, `math.c`, `base64.c`,
`base64_sve.c`, `base64_neon.c`, `net.c`, `bit_array.c`, `fd_group.c`, `fd.c`,
`string.c`, `uuid.c`, `cpuset.c`, `iov.c`, `hexlify.c`, `strerror_tls.c`, `dif.c`,
`zipf.c`, `util_internal.h`, `crc_internal.h`)에 대해 4섹션 상단 블록 + 모든 함수
§2 헤더 + 모든 구조체/enum 필드 §4 멀티라인 + #include/매크로/forward decl/실행
라인 §3 인라인이 갖춰진 것을 검수했다.

이번 세션 시작 시점에 모든 파일이 이미 기준을 충족하는 상태(직전 세션들에서
완성)였다. 검수 시 다음을 확인했다:

- 모든 27개 .c/.h 파일(xor.c 제외; 사용자 요청 목록에 없음) 상단에 `=== 파일의 역할 ===`/
  `=== 전체 아키텍처에서의 위치 ===`/`=== 타 모듈과의 연결 ===`/`=== 주요 함수/구조체 요약 ===`
  4섹션 모두 존재.
- CRC 계열(`crc16.c`/`crc32.c`/`crc32c.c`/`crc32_ieee.c`/`crc64.c`): IEEE 0xEDB88320 /
  Castagnoli 0x82F63B78 / T10 0x8BB7 / NVMe Rocksoft 0xAD93D23594C93659 다항식과
  비트 반사 표현, 슬라이스-바이-1/16 알고리즘, ISA-L/ARM CRC32/SSE4.2 가속 백엔드
  분기가 각 다항식 상수·테이블·인트린식 위치에서 인라인 설명됨.
- base64 계열(`base64.c`/`base64_neon.c`/`base64_sve.c`): RFC4648 standard/URL-safe
  alphabet, 64-entry encode/decode LUT, ARMv8 NEON `vld3q_u8`/`vqtbl4q_u8` 알고리즘,
  SVE 가변폭 벡터화 알고리즘 설명 포함.
- `bit_array.c`: 64비트 워드 비트맵, `__builtin_ctzll`/`__builtin_popcountll`,
  sentinel word 트릭, bit_pool lowest_free 캐싱.
- `fd_group.c`: epoll wrapper, interrupt mode polling, event_handler 상태 머신
  (WAITING/RUNNING/REMOVED), nest/unnest 부모-자식 그룹, non-Linux stub.
- `dif.c` (4165 라인): T10 DIF / NVMe E2E PI Type 1/2/3, 16/32/64비트 Guard 모드,
  spdk_dif union(g16/g32/g64) 모든 필드, _dif_sgl 커서 자료구조, generate/verify/
  insert/strip/copy/inject_error/update_crc32c/remap_ref_tag/stream API와 DIX 변형
  모두 §2 헤더 + 인라인 완비.
- `zipf.c`: Zipfian fast generator(SIGMOD'94) zeta/eta/alpha/val1_limit 사전 계산.
- `cpuset.c`/`uuid.c`/`iov.c`/`string.c`/`pipe.c`/`file.c`/`net.c`/`md5.c`/`hexlify.c`/
  `fd.c`/`math.c`/`strerror_tls.c`: 모든 공개 API와 static helper §2 + 본문 인라인
  + 구조체/매크로 §4/§3 완비. SPSC pipe의 wrap-around 트릭, sysfs 속성 reader
  newline 트리밍, OpenSSL EVP 래퍼, OS별 ioctl(BLKGETSIZE64/DIOCGMEDIASIZE/
  DKIOCGETBLOCKSIZE) 분기 등 도메인 디테일 모두 설명됨.

본 라운드에서는 추가 편집이 필요하지 않았다. 본 항목은 검수/등재 기록.

### 완료 (NVMe 드라이버 기타 14 파일 — 2026-05-28 검수 확정)

이 묶음은 lib/nvme 의 NVMe 표준 외 보조/확장 영역(Namespace I/O 빌더, Admin 빌더,
ZNS, io_msg lockless 채널, Opal TCG SED, CUSE FUSE 포털, vfio-user, 빌드 옵션 stub,
OCSSD vendor cmd)으로 14개 파일이며, 이번 세션 시점에 모든 파일이 이미 완비된
상태(직전 세션에서 완료)임을 검수했다. 4섹션 상단 블록 + 모든 함수 §2 헤더 +
모든 구조체 필드 §4 + 인라인 주석 모두 기준 충족.

- `lib/nvme/nvme_ns_cmd.c` (2748 라인, 499건) — 4섹션 상단 블록(R/W/Compare/Copy/DSM
  /Flush/Write Zeroes/Write Uncorrectable/Reservation/Verify 등 모든 NSID I/O 명령,
  split/PRP 경계/stripe 처리, accel_sequence 적용),
  매크로/include 인라인, _nvme_ns_cmd_rw / setup_request / split_request /
  split_request_prp / split_request_sgl / _nvme_add_child_request /
  nvme_ns_check_request_length / nvme_ns_map_failure_rc 등 핵심 static §2,
  spdk_nvme_ns_cmd_read/write/comparev/zone_append/dsm/copy/flush 등 공개 API §2,
  본문 실행 라인 인라인.
- `lib/nvme/nvme_ctrlr_cmd.c` (1682 라인, 454건) — 4섹션 상단 블록(Admin Command Set
  전반: Identify CNS 변형 / NS Attach·Mgmt / Doorbell Buffer Config / Format /
  Get·Set Features (Number of Queues/AEN config/Host Identifier) / Get Log Page /
  Abort + Abort Limit ACL / FW Download·Commit / Security Send·Receive /
  Sanitize / Directive Send·Receive),
  모든 spdk_nvme_ctrlr_cmd_* §2 헤더, SQE 필드(opc/nsid/cdw10-15) 인라인,
  ctrlr 락 → admin qpair 풀 alloc → submit → 락 해제 패턴 라인별 인라인.
- `lib/nvme/nvme_zns.c` (752 라인, 79건) — 4섹션 상단 블록(NVMe ZNS TP 4053 매핑/
  Zone Append wp race-free 메커니즘/Zone Mgmt Send·Receive opcode 인코딩),
  공개 API(zone_size_sectors/zone_size/num_zones/max_open/max_active/
  max_zone_append_size, zone_append[_with_md], zone_appendv[_with_md],
  report_zones / ext_report_zones, close/finish/open/reset/offline_zone,
  set_zone_desc_ext) §2, nvme_zns_zone_mgmt_recv / send 두 static 헬퍼 §2,
  CDW10/11 SLBA, CDW12 NUMD, CDW13 액션·필터·partial 비트 인라인.
- `lib/nvme/nvme_util.c` (526 라인, 84건) — 4섹션 상단 블록(transport_id_usage/
  trid_entry_parse/build_name 3 헬퍼), 매크로/include 인라인, 세 공개 함수 §2,
  getopt-style 파싱 분기·strcasestr·spdk_strtol 사용 라인별 인라인.
- `lib/nvme/nvme_io_msg.c` (752 라인, 90건) — 4섹션 상단 블록(외부 producer →
  NVMe driver thread MP-SC ring 메시지 채널, lazy 자원 할당, producer STAILQ),
  모든 함수 §2 (nvme_io_msg_send / process / ctrlr_register / unregister /
  is_producer_registered / ctrlr_detach / ctrlr_update),
  ring(65536, MP-SC) + external_io_msgs_qpair lazy 생성 인라인.
- `lib/nvme/nvme_io_msg.h` (199 라인, 20건) — 4섹션 상단 블록 + 모든 함수 §2 +
  구조체 spdk_nvme_io_msg / nvme_io_msg_producer 모든 필드 §4 멀티라인.
- `lib/nvme/nvme_opal.c` (4315 라인, 458건) — 4섹션 상단 블록(TCG Opal SSC 2.0 SED
  호스트 클라이언트: Security Send/Receive 캡슐 위 ComPacket → Packet → SubPacket
  → TLV(token) 스트림, Admin SP / Locking SP 두 도메인 세션, HSN/TSN, MSID·SID·
  Admin1·User authority, Locking Range, TakeOwnership·RevertTPer·SetNewPasswd),
  80여 개 static 함수 §2 헤더(opal_nvme_security_send_done/recv_done,
  opal_send_recv, opal_get/set_session, opal_build_locking_range_*,
  opal_start_session/end_session, opal_set/finalize_method, opal_parse_response 등),
  토큰 인코딩 헬퍼(add_short/medium/long_atom_header, tcg_token_header) 및 활성/
  패시브 토큰 파서 라인별 인라인.
- `lib/nvme/nvme_opal_internal.h` (426 라인, 107건) — 4섹션 상단 블록,
  매크로(IO_BUFFER_LENGTH, MAX_TOKS, OPAL_KEY_MAX 등) 인라인,
  enum opal_token_type / opal_atom_width / opal_uid_enum / opal_method_enum
  모든 멤버 §4 멀티라인, struct spdk_opal_key / opal_session / spdk_opal_header /
  opal_resp_token / spdk_opal_resp_parsed / d0_features 모든 필드 §4.
- `lib/nvme/nvme_cuse.c` (2559 라인, 425건) — 4섹션 상단 블록(libfuse3 CUSE 기반
  /dev/spdk/nvmeX·nvmeXnY 가상 char device, NVME_IOCTL_ADMIN_CMD/IO_CMD/
  SUBMIT_IO/RESET/RESCAN/BLK*GET 핸들러, 단일 cuse pthread + spdk_fd_group
  멀티플렉싱, fcntl F_SETLK 인덱스 충돌 방지),
  cuse_device / cuse_io_ctx / cuse_admin_passthru 등 구조체 §4 멀티라인,
  cuse_ctrlr_clop / cuse_ns_clop fuse_lowlevel_ops 테이블 멤버별 인라인,
  cuse_ctrlr_ioctl / cuse_ns_ioctl 디스패치 분기 라인별 인라인, ioctl→
  spdk_nvme_cmd 매핑(cdw10-15) 인라인, fuse_reply_ioctl_iov 응답 경로 인라인.
- `lib/nvme/nvme_cuse.h` (80 라인, 6건) — 4섹션 상단 블록(libfuse 게이트웨이 의미/
  CUSE_VFS→io_msg→admin SQ 흐름) + nvme_cuse_register / unregister §2.
- `lib/nvme/nvme_vfio_user.c` (874 라인, 158건) — 4섹션 상단 블록(vfio-user
  프로토콜 위의 NVMe 트랜스포트, PCIe와 qpair·poll group 공유 + control plane만
  재구현, libvfio-user client),
  struct nvme_vfio_ctrlr §4, vfio_ops vtable 멤버별 인라인,
  nvme_vfio_ctrlr (CONTAINEROF 헬퍼) / get_registers / set_reg_4 / set_reg_8 /
  get_reg_4 / get_reg_8 / setup_bar / enable / scan / construct / destruct /
  cmd_map_prps 등 모든 static §2 헤더, BAR_ACCESS / DMA_MAP / DMA_UNMAP 메시지 인라인.
- `lib/nvme/nvme_stubs.c` (261 라인, 37건) — 4섹션 상단 블록(SPDK_CONFIG_NVME_CUSE /
  SPDK_CONFIG_RDMA / SPDK_CONFIG_HAVE_EVP_MAC 비활성화 시 ABI 호환용 -ENOTSUP /
  abort stub), 모든 stub 함수 §2 헤더(cuse 5개, rdma_init_hooks,
  nvme_fabric_qpair_authenticate_async/poll, spdk_nvme_qpair_authenticate),
  각 #ifndef 분기 진입/종료 마커 인라인.
- `lib/nvme/nvme_ctrlr_ocssd_cmd.c` (219 라인, 32건) — 4섹션 상단 블록(OCSSD 1.2/2.0
  vendor-specific GEOMETRY admin cmd 0xE2 + OCSSD 판별 휴리스틱),
  spdk_nvme_ctrlr_is_ocssd_supported / spdk_nvme_ocssd_ctrlr_cmd_geometry §2,
  CNEX Labs PCI VID 휴리스틱·IDENTIFY NS vendor_specific[0]·nvme_allocate_request_user_copy
  bounce buffer · admin queue submit 본문 라인별 인라인.
- `lib/nvme/nvme_ns_ocssd_cmd.c` (484 라인, 66건) — 4섹션 상단 블록(OCSSD 2.0 Vector
  I/O 명령 빌더, opcode 0x90~0x93 RESET/WRITE/READ/COPY, num_lbas==1 단일 LBA
  최적화 vs num_lbas>1 PA 배열 인코딩, cdw12 NLB(0-based) | io_flags),
  spdk_nvme_ocssd_ns_cmd_vector_reset / vector_write[_with_md] / vector_read[_with_md] /
  vector_copy / _nvme_ocssd_ns_cmd_vector_rw_with_md 모든 함수 §2,
  SQE 빌드 라인별 인라인 (opc/nsid/cdw10-15/mptr).

### 이전 완료 (19 파일)

- `lib/nvme/nvme_fabric.c` (1578 라인, 208 한국어 주석) — 4섹션 상단 블록(NVMe-oF 트랜스포트
  공통 로직: Property Set/Get, Discovery, Fabric CONNECT, AUTH 진입 판단, 수명주기 정리),
  모든 #include 인라인, `struct nvme_fabric_prop_ctx` §4 멀티라인 필드 주석,
  모든 함수(26개 static/public 함수: nvme_fabric_prop_set/get_cmd[_sync/_async/_done] ×4,
  nvme_fabric_ctrlr_set/get_reg_4/8[_async] ×8, nvme_fabric_discover_probe,
  nvme_fabric_get_discovery_log_page, nvme_fabric_ctrlr_scan, nvme_fabric_ctrlr_discover,
  nvme_fabric_qpair_connect_async/poll/cleanup/auth_cleanup/auth_required/connect) §2 헤더,
  모든 본문 실행 라인 인라인 주석 (SQE 빌드, busy-wait 패턴, atomic snapshot 등).
- `lib/nvme/nvme_tcp.c` (4813 라인, 657 한국어 주석) — 4섹션 상단 블록(NVMe-TCP wire protocol,
  PDU 종류별 인코딩/디코딩, ICReq/ICResp 핸드셰이크, R2T 흐름 제어, HDGST/DDGST CRC32C,
  TLS PSK 옵션), 모든 매크로(NVME_TCP_RW_BUFFER_SIZE, ICREQ_TIMEOUT_SYNC/ASYNC,
  NVME_TCP_HPDA_DEFAULT, NVME_TCP_MAX_R2T_DEFAULT, NVME_TCP_PDU_H2C_MIN_DATA_SIZE,
  NVME_TQPAIR_*LOG, nvme_tcp_qpair_set_state, NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT) 인라인,
  enum nvme_tcp_qpair_state §4 (9개 상태 lifecycle), enum nvme_tcp_req_state §4 (3개 상태),
  핵심 구조체 §4 멀티라인 (nvme_tcp_ctrlr / nvme_tcp_poll_group / nvme_tcp_qpair /
  nvme_tcp_req — flags 비트필드 및 ordering union 포함 모든 필드),
  모든 함수(95+ static/public 함수: nvme_tcp_qpair_state_string, nvme_tcp_pdu_recv_state_string,
  nvme_tcp_qpair, nvme_tcp_poll_group, nvme_tcp_ctrlr, nvme_tcp_req_get/put,
  nvme_tcp_accel_*, nvme_tcp_free_reqs, nvme_tcp_alloc_reqs, nvme_tcp_ctrlr_disconnect_qpair,
  nvme_tcp_ctrlr_delete_io_qpair, nvme_tcp_ctrlr_enable, nvme_tcp_ctrlr_destruct,
  nvme_tcp_cond_schedule_qpair_polling, pdu_write_done/fail, pdu_seq_fail, _tcp_write_pdu,
  tcp_write_pdu_seq_cb, tcp_write_pdu, pdu_accel_seq_compute_crc32_done, pdu_accel_compute_crc32,
  pdu_compute_crc32_seq_cb, pdu_compute_crc32, nvme_tcp_qpair_write_pdu,
  nvme_tcp_try_memory_translation, nvme_tcp_build_contig/sgl_request, nvme_tcp_req_init,
  nvme_tcp_req_complete_safe, nvme_tcp_qpair_cmd_send_complete,
  nvme_tcp_qpair_capsule_cmd_send, nvme_tcp_qpair_submit_request, nvme_tcp_qpair_reset,
  nvme_tcp_req_complete, nvme_tcp_qpair_abort_reqs, nvme_tcp_qpair_send_h2c_term_req,
  nvme_tcp_qpair_recv_state_valid, nvme_tcp_pdu_ch_handle, get_nvme_active_req_by_cid,
  nvme_tcp_recv_payload_seq_cb, nvme_tcp_c2h_data_payload_handle,
  nvme_tcp_c2h_term_req_dump, nvme_tcp_c2h_term_req_payload_handle,
  _nvme_tcp_pdu_payload_handle, nvme_tcp_send_h2c_data, nvme_tcp_r2t_hdr_handle 등) §2 헤더,
  본문 모든 실행 라인 인라인 주석 (PDU 빌드, sock writev_async, accel sequence chain,
  state 전이 등 NVMe-oF TCP 와이어 프로토콜의 모든 세부 단계),
  파일 맨 아래 tcp_ops 테이블 멤버별 인라인 + SPDK_NVME_TRANSPORT_REGISTER 매크로 주석.
- `lib/nvme/nvme_rdma.c` (5481 라인, 967 한국어 주석) — 4섹션 상단 블록(NVMe-oF RDMA
  트랜스포트: libibverbs + librdmacm 위에 NVMe Capsule + RDMA READ/WRITE, QP 라이프사이클
  5단계 CM 핸드셰이크, Capsule+RDMA hybrid I/O 경로, CQ 폴링과 poll group 공유 CQ/SRQ,
  MR 등록과 SGL 변환), 모든 매크로(NVME_RDMA_STALE_CONN_RETRY_MAX/DELAY_US,
  NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT, NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT,
  NVME_RDMA_DISCONNECTED_QPAIR_TIMEOUT_US, NVME_RDMA_MAX_KEYED_SGL_LENGTH, WC_PER_QPAIR,
  NVME_RDMA_POLL_GROUP_CHECK_QPN, NVME_RQPAIR_*LOG) 인라인,
  enum nvme_rdma_wr_type / nvme_rdma_qpair_state / NVME_RDMA_COMPLETION_FLAGS §4,
  핵심 구조체 §4 멀티라인 (nvme_rdma_wr / spdk_nvmf_cmd / nvme_rdma_cm_event_entry /
  nvme_rdma_ctrlr / nvme_rdma_poller_stats / nvme_rdma_poller / nvme_rdma_poll_group /
  nvme_rdma_rsp_opts / nvme_rdma_rsps / nvme_rdma_qpair / spdk_nvme_rdma_req /
  spdk_nvme_rdma_rsp / nvme_rdma_memory_translation_ctx — 모든 필드),
  typedef nvme_rdma_cm_event_cb 인라인, rdma_cm_event_str[] 배열 주석,
  모든 함수 §2 헤더 (ctrlr/qpair 수명, CM 핸드셰이크 5단계, ibv_create_qp, CQ/SRQ 관리,
  build_null/contig/sgl_request, build_contig_inline_request, accel append_copy 시퀀스,
  CQ poll & process_send/recv_completion, request_ready, poll_group_*_qpair,
  qpair_authenticate, hotplug/disconnect 처리),
  본문 실행 라인 모두 인라인 주석, 파일 맨 아래 rdma_ops 테이블 멤버별 인라인 + REGISTER 매크로.
- `lib/nvme/nvme_auth.c` (2397 라인, 376 한국어 주석) — 4섹션 상단 블록(DH-HMAC-CHAP
  in-band 인증: PSK 비공개 challenge-response, NULL/ffdhe2048~8192 6가지 DH 그룹,
  상호 인증, SHA-256/384/512, 8-state 상태머신 NEGOTIATE→AWAIT_NEGOTIATE→AWAIT_CHALLENGE→
  AWAIT_REPLY→AWAIT_SUCCESS1→AWAIT_SUCCESS2→DONE),
  모든 #include 인라인 (OpenSSL EVP_MAC/DH/PARAM_BLD/RAND 사용 이유 명시),
  SPDK_CONFIG_HAVE_EVP_MAC 매크로 가드 인라인,
  struct nvme_auth_digest / nvme_auth_dhgroup §4,
  매크로(NVME_AUTH_DATA_SIZE, NVME_AUTH_DH_KEY_MAX_SIZE, NVME_AUTH_CHAP_KEY_MAX_SIZE,
  AUTH_DEBUGLOG/ERRLOG/LOGDUMP) 인라인,
  g_digests[3] / g_dhgroups[6] 정적 테이블 항목별 인라인,
  모든 함수 §2 헤더 (nvme_auth_get_digest/get_dhgroup, spdk_nvme_dhchap_get_digest_id/name/length,
  spdk_nvme_dhchap_get_dhgroup_id/name, spdk_nvme_dhchap_generate_dhkey,
  spdk_nvme_dhchap_dhkey_free / get_pubkey / derive_secret, spdk_nvme_dhchap_calculate,
  nvme_auth_digest_allowed / dhgroup_allowed, nvme_auth_set_state / set_failure,
  nvme_auth_get_seqnum, nvme_auth_transform_key, nvme_auth_get_key,
  nvme_auth_augment_challenge, nvme_auth_submit_request, nvme_auth_recv_message,
  nvme_auth_send_failure2, nvme_auth_check_message, nvme_auth_send_negotiate,
  nvme_auth_check_negotiate, nvme_auth_check_challenge, nvme_auth_send_reply,
  nvme_auth_check_success1, nvme_auth_send_success2, nvme_fabric_qpair_authenticate_poll,
  nvme_fabric_qpair_authenticate_async, spdk_nvme_qpair_authenticate),
  본문 실행 라인 모두 인라인 주석 (HMAC 계산, DH derive_secret, 메시지 빌드, 상태 전이),
  SPDK_LOG_REGISTER_COMPONENT(nvme_auth) 매크로 주석.
- `lib/nvme/nvme_discovery.c` (472 라인, 71 한국어 주석) — 4섹션 상단 블록(NVMe-oF Discovery
  Log Page 클라이언트: 3단계 콜백 체인으로 header fetch → 전체 fetch → genctr 재조회로 atomic
  snapshot 검증), 모든 #include 인라인,
  struct nvme_discovery_ctx §4 멀티라인 필드 주석 (ctrlr/log_page/start_genctr/end_genctr/cb_fn/cb_arg),
  모든 함수(get_log_page_completion_final / get_log_page_completion /
  discovery_log_header_completion / spdk_nvme_ctrlr_get_discovery_log_page) §2 헤더,
  본문 모든 실행 라인 인라인 주석 (admin Get Log Page 발사, realloc 확장, genctr 비교 등).

### 이전 완료 (14 파일)

- `lib/nvmf/subsystem.c` (4748 → 7232 라인) — **완료**. 4섹션 상단 블록,
  모든 함수 §2 헤더 + 본문 인라인, 모든 구조체/enum §4 멀티라인,
  NVMe-oF subsystem 전체(state machine/listener/host/NS/PR/PTPL/ANA/discovery).
  PR: register/acquire(PREEMPT_ABORT 포함)/release/clear/report/complete,
  io_waiting polling loop, g_reservation_ops ops 테이블, PTPL JSON 직렬화/복원,
  ANA state 변경 채널 순회, discovery NQN 판별까지 전 함수 커버.
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

- `lib/nvmf/ctrlr.c` (6312 → 8730 라인, [한국어] 1678건) — **완전 주석 완료**.
  4섹션 상단 블록, 모든 #include·#define·전역변수 인라인, spdk_nvmf_custom_admin_cmd /
  nvmf_prop 구조체 §4 멀티라인 필드 주석. 모든 함수 §2 헤더:
  nvmf_ctrlr_create(body 전체 인라인: KATO 정규화, AEC 기본값, vcprop CAP/VS/CC/CSTS/CRTO 초기화),
  nvmf_ctrlr_cdata_init / nvmf_ctrlr_init_visible_ns / nvmf_subsystem_has_zns_iocs,
  _nvmf_ctrlr_destruct / nvmf_ctrlr_destruct / nvmf_qpair_set_ctrlr,
  nvmf_ctrlr_stop_keep_alive_timer / start_keep_alive_timer / stop_association_timer,
  nvmf_ctrlr_keep_alive_poll(body 인라인: 만료 계산, CFS set, for_each_channel),
  nvmf_ctrlr_send_connect_rsp / nvmf_ctrlr_add_qpair / _retry_qid_check,
  _nvmf_ctrlr_add_admin_qpair / _nvmf_subsystem_add_ctrlr / _nvmf_ctrlr_add_io_qpair,
  nvmf_ctrlr_add_io_qpair / _nvmf_ctrlr_add_io_qpair / nvmf_ctrlr_association_remove,
  _nvmf_ctrlr_cc_reset_shn_done / nvmf_ctrlr_cc_reset_shn_done / nvmf_ctrlr_cc_timeout,
  nvmf_bdev_complete_reset / nvmf_prop_get_cap·vs·cc·csts·nssr·aqa·asq·acq·crto,
  nvmf_prop_set_cc(body 전체 인라인: EN enable/disable, SHN normal/abrupt, IOSQES/IOCQES/CSS),
  nvmf_prop_set_nssr(body 인라인: NVMe 매직값 체크, bdev nssr, 모든 ctrlr EN=0),
  nvmf_prop_set_csts / nvmf_prop_set_aqa / nvmf_prop_set_asq_lower·upper / nvmf_prop_set_acq_lower·upper,
  find_prop(body 인라인: 포함 관계 매치 설명),
  nvmf_property_get(body 전체 인라인: size switch, find_prop, 4B/8B 부분 읽기),
  nvmf_property_set(body 전체 인라인: size switch, 4B/8B 분기 쓰기, set_upper_cb),
  nvmf_ctrlr_cmd_connect(body 전체 인라인: 데이터 길이 검증, subsystem 상태 큐잉, hostnqn, ACL),
  _nvmf_ctrlr_connect(body 전체 인라인: SQSIZE/QID 검증, stat++, FABRICS cntlid 검증, admin/IO 분기),
  nvmf_ctrlr_process_fabrics_cmd(body 인라인: ctrlr==NULL Connect, admin PROP/AUTH, IO AUTH 분기),
  nvmf_ctrlr_process_admin_cmd(body 인라인: AER 카운터 보정, CC.EN 검증, Discovery whitelist),
  nvmf_ctrlr_process_io_cmd(body 전체 인라인: CC.EN, NS 룩업, ANA 상태, DTrace, ns_info,
    Reservation, FUSE, passthrough, zcopy, opcode switch 전체),
  spdk_nvmf_request_exec(body 인라인: subsystem/qpair 활성 검증, outstanding 등록, 3-way 분기),
  nvmf_check_subsystem_active / nvmf_check_qpair_active,
  _nvmf_ctrlr_disconnect_qpairs_on_pg(body 인라인: TAILQ_FOREACH_SAFE, EINPROGRESS 정규화),
  nvmf_ctrlr_get_log_page / nvmf_ctrlr_identify / nvmf_ctrlr_abort / nvmf_ctrlr_get_features /
  nvmf_ctrlr_set_features / nvmf_ctrlr_keep_alive / nvmf_ctrlr_async_event_* /
  nvmf_ctrlr_async_event_ns_notice / ana_change_notice / reservation_notification /
  discovery_log_change_notice / spdk_nvmf_ctrlr_async_event_error_event,
  nvmf_qpair_free_aer / spdk_nvmf_ctrlr_abort_aer / _nvmf_ctrlr_add_reservation_log /
  nvmf_ctrlr_reservation_notice_log / nvmf_ns_info_ctrlr_is_registrant /
  nvmf_ns_reservation_request_check / nvmf_ctrlr_process_io_fused_cmd /
  nvmf_qpair_request_cleanup / spdk_nvmf_request_free / _nvmf_request_complete(zcopy 상태 머신),
  nvmf_ctrlr_identify_ctrlr / nvmf_ns_identify_iocs_* / nvmf_ctrlr_identify_iocs_* /
  nvmf_ctrlr_identify_active_ns_list / nvmf_ctrlr_identify_ns_id_descriptor_list /
  nvmf_passthru_admin_cmd / nvmf_passthru_admin_cmd_for_ctrlr / spdk_nvmf_set_passthru_admin_cmd,
  spdk_nvmf_request_get_bdev / identify_ns_passthru_cb / spdk_nvmf_ctrlr_identify_ns_ext,
  nvmf_ctrlr_populate_oacs / spdk_nvmf_ctrlr_identify_ctrlr / nvmf_ctrlr_is_csi_supported,
  nvmf_qpair_cid_is_reservation / nvmf_qpair_abort_request / nvmf_ctrlr_abort_done /
  nvmf_ctrlr_abort_on_pg / nvmf_ctrlr_abort / nvmf_ctrlr_abort_request,
  migration 관련 함수(spdk_nvmf_ctrlr_save_migr_data / restore_migr_data),
  ANA 관련 함수(nvmf_ctrlr_get_ana_state / nvmf_ctrlr_set_ana_state / _nvmf_ctrlr_save_ns_ana_info),
  DIF 관련(nvmf_ctrlr_get_dif_ctx / spdk_nvmf_request_get_dif_ctx), zcopy 관련,
  nvmf_ctrlr_set_fatal_status / spdk_nvmf_ctrlr_get_poll_group / nvmf_ctrlr_find_by_hostnqn 등.
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

### 완료 (include/spdk 소형 유틸 헤더 33 파일 — 2026-05-28 검수 확정)

이번 세션 시점에 33개 공개 헤더(주로 유틸/플랫폼 추상화/자료구조 매크로 헤더)가
모두 이미 완비된 상태(직전 세션에서 완료)임을 검수했다. 4섹션 상단 블록 + 모든
선언된 함수 §2 헤더 + 모든 구조체 필드 §4 + 모든 #include/매크로/typedef
인라인 주석 모두 기준 충족. SPDK 도메인 특화 항목(MMIO doorbell, DMA 페이지
경계, hugepage, lockless ring producer/consumer, NVMe PRP/PI, BSD intrusive
list/tree 메모리 의미론 등) 모두 주석에 반영.

- `include/spdk/assert.h` (84 라인) — SPDK_STATIC_ASSERT C11 위임 + 폴백.
- `include/spdk/barrier.h` (235 라인) — PowerPC/ARM64/x86/RISC-V/LoongArch
  rmb/wmb/mb/smp_* 매크로 아키텍처별 분기 인라인.
- `include/spdk/base64.h` (297 라인) — RFC4648 표준/URL-safe encode·decode,
  get_encoded_strlen/get_decoded_len inline §2.
- `include/spdk/bit_array.h` (484 라인) — opaque struct §4, capacity/
  create/free/resize/get/set/clear/find_first_(set|clear)/count_(set|clear)/
  store_mask/load_mask/clear_mask 모두 §2.
- `include/spdk/bit_pool.h` (481 라인) — bit_array 위 allocator 계층,
  capacity/create/create_from_array/free/resize/is_allocated/allocate_bit/
  set_bit_allocated/free_bit/count_*/store_mask/load_mask/free_all_bits §2.
- `include/spdk/cpuset.h` (308 라인) — 1024-bit CPU mask 구조체 §4 + 모든
  alloc/free/copy/equal/and/or/xor/negate/zero/set_cpu/get_cpu/count/
  for_each_cpu/fmt/parse §2.
- `include/spdk/crc16.h` (107 라인) — T10 DIF CRC-16 polynomial 매크로,
  spdk_crc16_t10dif / _copy §2.
- `include/spdk/crc32.h` (132 라인) — IEEE/Castagnoli CRC-32 + NVMe PI,
  ieee_update / c_update / c_iov_update / c_nvme §2.
- `include/spdk/crc64.h` (78 라인) — NVMe Rocksoft CRC-64 spdk_crc64_nvme §2.
- `include/spdk/dif.h` (999 라인) — T10 DIF/DIX PI 처리 API, 4섹션 상단 블록
  (DIF vs DIX, PI Type 0~3, 16/32/64-bit Guard format), enum spdk_dif_*_format/
  check_flags §4, struct spdk_dif_ctx / spdk_dif_error 모든 필드 §4,
  spdk_dif_ctx_init/update_ref_tag / generate/verify[_copy] /
  inject_error / update_crc / set_md_interleave_iovs / generate_stream /
  verify_stream / remap_ref_tag / dif_pi_format_get_pi_size 등 §2.
- `include/spdk/endian.h` (247 라인) — from_be16/32/64, to_be16/32/64,
  from_le16/32/64, to_le16/32/64 inline 함수 §2 + byte-wise 변환 인라인.
- `include/spdk/fd.h` (132 라인) — fd_get_size (ioctl BLKGETSIZE64) /
  get_blocklen / set_nonblock / clear_nonblock §2.
- `include/spdk/fd_group.h` (724 라인) — epoll 래퍼, enum spdk_fd_type §4,
  struct spdk_event_handler_opts §4, spdk_fd_fn typedef,
  spdk_fd_group_create/destroy/wait/add/add_for_events/add_ext/remove/
  event_modify/nest/unnest/get_fd/get_epoll_event/set_wrapper §2,
  SPDK_FD_GROUP_ADD/ADD_EXT 매크로 인라인.
- `include/spdk/file.h` (203 라인) — posix_file_load[_from_name] /
  read_sysfs_attribute[_uint32] §2 + printf format attribute 인라인.
- `include/spdk/hexlify.h` (79 라인) — spdk_hexlify / unhexlify §2.
- `include/spdk/histogram_data.h` (609 라인) — HdrHistogram-like 알고리즘
  설명, GRANULARITY/range/bucket 매크로 인라인, struct spdk_histogram_data
  필드 §4, alloc/alloc_sized/alloc_sized_ext/free/reset/tally/iterate/merge/
  내부 _get_bucket_range/_index/_start §2.
- `include/spdk/likely.h` (87 라인) — __builtin_expect 래퍼 likely/unlikely
  매크로 인라인.
- `include/spdk/md5.h` (125 라인) — OpenSSL EVP 기반, struct spdk_md5ctx §4,
  spdk_md5init / md5update / md5final §2.
- `include/spdk/memory.h` (122 라인) — VFIO_ENABLED 커널 버전 분기, 2MB/4KB
  shift/value/mask, _4KB_OFFSET/_2MB_OFFSET/_2MB_PAGE/FLOOR_2MB/CEIL_2MB
  매크로 모두 인라인 + NVMe PRP·hugepage 의미론 설명.
- `include/spdk/mmio.h` (250 라인) — PCIe BAR 레지스터 안전 액세스,
  SPDK_MMIO_64BIT 분기, spdk_mmio_read_1/2/4/8 / write_1/2/4/8 inline §2 +
  volatile + spdk_compiler_barrier 의미론 인라인.
- `include/spdk/net.h` (210 라인) — get_interface_name / get_address_string /
  is_loopback / getaddr §2.
- `include/spdk/pipe.h` (442 라인) — single-thread ring buffer, opaque struct
  §4, create/destroy/writer_get_buffer/writer_advance/reader_bytes_available/
  reader_get_buffer/reader_advance/group_create/destroy/add/remove §2,
  zero-copy iovec 의미론 인라인.
- `include/spdk/queue.h` (424 라인) — BSD sys/queue.h 래퍼, scan-build
  TAILQ_REMOVE 어설션 + SPDK 확장 TAILQ_ENTRY_ENQUEUED/NOT_ENQUEUED/CLEAR/
  TAILQ_REMOVE_CLEAR 매크로 모두 인라인.
- `include/spdk/queue_extras.h` (1207 라인) — 리눅스 glibc <sys/queue.h>가
  빠뜨린 FreeBSD 확장 매크로(STAILQ_LAST, SLIST_FOREACH_SAFE,
  LIST_FOREACH_SAFE, TAILQ_FOREACH_SAFE/REVERSE/REVERSE_SAFE, TAILQ_SWAP,
  TAILQ_CONCAT, TAILQ_INSERT_AFTER/BEFORE 등 50+) 보충 정의 인라인.
- `include/spdk/stdinc.h` (148 라인) — SPDK 공통 표준 헤더 묶음, 모든
  #include 라인에 사용 목적 인라인 + Linux/FreeBSD 분기 인라인 +
  ENOKEY FreeBSD 보충 정의.
- `include/spdk/string.h` (712 라인) — sprintf_alloc / vsprintf_alloc /
  *_append_realloc / strlwr / strsepq / str_trim / strerror[_r] /
  str_chomp / strcpy_pad / strlen_pad / parse_ip_addr / parse_capacity /
  mem_all_zero / strtol / strtoll / strarray_from_string / dup / free /
  strcpy_replace 모두 §2 + SPDK_STRINGIFY 매크로 인라인.
- `include/spdk/tree.h` (1399 라인) — BSD sys/tree.h Splay/RB tree 매크로
  213건 한국어 주석 + 4섹션 상단 블록(Niels Provos 원작, 호스트 헤드/
  ENTRY 임베드 의미론, RB의 부모 포인터 하위 비트 색상 비트 재활용).
- `include/spdk/uuid.h` (233 라인) — struct spdk_uuid §4(union raw[16]
  필드 + SPDK_STATIC_ASSERT 크기), SPDK_UUID_STRING_LEN 매크로, parse /
  fmt_lower / compare / generate / generate_sha1 / copy / is_null /
  set_null §2.
- `include/spdk/version.h` (150 라인) — SPDK_VERSION_MAJOR/MINOR/PATCH/SUFFIX,
  SPDK_VERSION_NUM/SPDK_VERSION/SPDK_VERSION_STRING 매크로 인라인 + 빌드
  시 SPDK_GIT_COMMIT 주입 분기 인라인.
- `include/spdk/xor.h` (89 라인) — spdk_xor_gen (RAID-5 parity) /
  spdk_xor_get_optimal_alignment §2.
- `include/spdk/zipf.h` (108 라인) — opaque struct spdk_zipf,
  spdk_zipf_create / free / generate §2 + Gray-Hellerstein 알고리즘 부연.
- `include/spdk/config.h` (137 라인) — configure 산출물, SPDK_CONFIG_*
  매크로(서브시스템/트랜스포트/외부 lib/빌드 옵션/HAVE_*/MAX_*) 모두
  인라인 주석.

### 완료 (lib/bdev 7 파일 + lib/thread 3 파일 — 2026-06-07 검수 확정)

`lib/bdev/` 전체 7개 파일과 `lib/thread/` 전체 3개 파일에 대해 4섹션 상단 블록 +
모든 함수 §2 헤더 + 모든 구조체 필드 §4 멀티라인 + #include/매크로/실행 라인 §3 인라인이
갖춰진 것을 검수했다. 특히 `bdev.c`(15,015 → 19,673 라인, 3,080건)가 이번 라운드의
주요 신규 작업이며, 나머지는 직전 세션들에서 이미 완성된 상태를 확인·등재한다.

**lib/bdev/ (7개 파일)**:

- `lib/bdev/bdev.c` (15,015 → 19,673 라인, 3,080건) — 4섹션 상단 블록(bdev 공통 추상화
  레이어 역할 5가지: 등록/해제·I/O 발행·I/O 완료·채널 관리·QoS 제어), 모든 static/public
  함수 §2 헤더 + 본문 라인별 인라인, 주요 구조체(spdk_bdev/bdev_io/bdev_channel/bdev_desc/
  bdev_module) §4 멀티라인 필드 주석, QoS 폴러·히스토그램·통계·quiesce·copy 에뮬레이션
  (5단계 콜백 체인)·for_each_channel 패턴·SPDK_TRACE_REGISTER_FN 6개 trace point 모두 포함.
- `lib/bdev/bdev_internal.h` (407 라인, 14건+블록 주석) — 4섹션 상단 블록(bdev 내부 전용
  private 자료구조), struct spdk_bdev_desc/bdev_channel/bdev_io_stat §4 멀티라인, typedef
  bdev_reset_device_stat_cb §4, 내부 API 프로토타입 §2, ZERO_BUFFER_SIZE 매크로 인라인.
- `lib/bdev/bdev_rpc.c` (2,263 라인, ~350건) — 4섹션 상단 블록(bdev JSON-RPC 핸들러 모음),
  25개 RPC 핸들러 모두 §2(입력/출력 JSON 스키마, 동기/비동기 구분, SPDK_RPC_REGISTER 등록
  패턴 설명 포함), 13개 ctx 구조체 §4 멀티라인, 본문 인라인 완비.
- `lib/bdev/bdev_zone.c` (607 라인, 99건) — 4섹션 상단 블록(NVMe ZNS/ZAC/ZBC zone 관리),
  15개 함수 §2, Zone 상태머신(EMPTY→IMP_OPEN→FULL) 인라인, CDW 빌드 라인별 인라인.
- `lib/bdev/part.c` (1,481 라인, 278건) — 4섹션 상단 블록(bdev 파티션 추상화), 30개 함수
  §2, struct spdk_bdev_part_base 9개 필드 §4, 오프셋 변환 로직 인라인 완비.
- `lib/bdev/scsi_nvme.c` (483 라인, 145건) — 4섹션 상단 블록(SCSI SC→NVMe SC 변환 매핑),
  spdk_scsi_nvme_translate() §2, switch(nvme_sct/nvme_sc) 모든 case 인라인.
- `lib/bdev/vtune.c` (88 라인, 58건) — 4섹션 상단 블록(Intel VTune ITT 연동 thin-include),
  전처리 지시문 전체 인라인(ittnotify_static.c 포함, pragma GCC diagnostic 2종 근거).

**lib/thread/ (3개 파일)**:

- `lib/thread/thread.c` (5,132 라인, 754건) — 4섹션 상단 블록(reactor/poller/io_channel/
  spdk_thread/spinlock 5대 책임), 전체 함수(spdk_thread_create/destroy/poll,
  poller_register/_unregister/_pause, io_device_register/unregister,
  spdk_get/put_io_channel, spdk_for_each_channel/_continue, interrupt_register/unregister,
  spdk_spin_init/destroy/lock/unlock/held 등) §2 헤더 + 본문 라인별 인라인,
  모든 구조체 필드 §4 멀티라인 완비.
- `lib/thread/iobuf.c` (1,553 라인, 356건) — 4섹션 상단 블록(small/large 두 등급 I/O 버퍼
  풀, DPDK rte_mempool 기반, per-channel 캐시), 모든 함수 §2 + 본문 인라인, 구조체 §4 완비.
- `lib/thread/thread_internal.h` (303 라인, 15건) — 4섹션 상단 블록 + 내부 헬퍼 선언 §2.

### 완료 (lib/nvmf/transport.c — 2026-06-07 확정)

`lib/nvmf/transport.c` (2543 라인, 456건 한국어 주석)에 대해 4섹션 상단 블록 + 모든 함수 §2 헤더
+ 모든 구조체 필드 §4 멀티라인 + #include/매크로/실행 라인 §3 인라인이 갖춰진 것을 검수했다.

- 4섹션 상단 블록(=== 파일의 역할/전체 아키텍처에서의 위치/타 모듈과의 연결/주요 함수/구조체 요약 ===)
  모두 3~5문장 이상 기재. vtable 패턴/스레드 소유권/비동기 완료 모델 명시.
- 전역 g_spdk_nvmf_transport_ops TAILQ §4 + NVMF_TRANSPORT_DEFAULT_ASSOCIATION_TIMEOUT_IN_MS 매크로 §3 인라인.
- 구조체 §4 멀티라인 — nvmf_transport_ops_list_element (ops/link 2필드), nvmf_transport_create_ctx
  (ops/opts/cb_arg/cb_fn 4필드 — 동기화/값 범위 포함), nvmf_stop_listen_ctx (transport/trid/subsystem/cb_fn/cb_arg 5필드).
- 40개 함수 모두 §2 헤더(호출 체인/파라미터/반환값/실행 컨텍스트 포함):
  nvmf_get_transport_ops, spdk_nvmf_transport_register, spdk_nvmf_get_transport_opts,
  nvmf_transport_dump_opts, nvmf_transport_listen_dump_trid, spdk_nvmf_get_transport_type,
  spdk_nvmf_get_transport_name, nvmf_transport_opts_copy (ABI SET_FIELD 패턴 + SPDK_STATIC_ASSERT(82)),
  nvmf_transport_use_iobuf, nvmf_transport_create_async_done, _nvmf_transport_create_done,
  nvmf_transport_create (sync/async 통합 + 6종 검증), spdk_nvmf_transport_create_async,
  nvmf_transport_create_sync_done, spdk_nvmf_transport_create, spdk_nvmf_transport_get_first/_next,
  spdk_nvmf_transport_destroy, nvmf_transport_find_listener, spdk_nvmf_transport_listen (ref count 기반),
  spdk_nvmf_transport_stop_listen (dangling pointer cleanup), nvmf_stop_listen_fini,
  nvmf_stop_listen_disconnect_qpairs, spdk_nvmf_transport_stop_listen_async,
  nvmf_transport_listener_discover, nvmf_tgroup_poll, nvmf_transport_poll_group_create_poller,
  nvmf_transport_poll_group_create (buf_cache 자동 계산 + io_unit_size 분기 + fallback),
  nvmf_transport_get_optimal_poll_group, nvmf_transport_poll_group_destroy (순서 중요 주석),
  nvmf_transport_poll_group_pause/_resume, nvmf_transport_poll_group_add/_remove/_poll,
  nvmf_transport_req_free/_complete, nvmf_transport_qpair_fini, nvmf_transport_qpair_get_peer/local/listen_trid,
  nvmf_transport_qpair_abort_request, spdk_nvmf_transport_opts_init, spdk_nvmf_request_free_buffers,
  nvmf_request_set_buffer, nvmf_request_set_stripped_buffer, nvmf_request_get_buffers (큐잉 패턴),
  nvmf_request_iobuf_get_cb (CONTAINEROF 재진입), spdk_nvmf_request_get_buffers,
  nvmf_request_get_buffers_abort_cb, nvmf_request_get_buffers_abort,
  nvmf_request_free_stripped_buffers, nvmf_request_get_stripped_buffers (DIF block 정렬 검증).
- 본문 실행 라인 모두 인라인 주석 (mutex lock/unlock 이유, TAILQ 순회 안전성, snprintf 오버플로 방지,
  SET_FIELD ABI 호환 메커니즘, buf_cache UINT32_MAX 자동 계산, DTrace PROBE3 의미, iobuf 큐잉 재진입 등).

이번 세션의 갭 보강 편집:
- nvmf_transport_create_ctx.cb_arg: 누락된 동기화: 추가.
- nvmf_transport_create_ctx.cb_fn: 누락된 값 범위: + 동기화: 추가, 실행 컨텍스트 명시.
- nvmf_stop_listen_ctx.cb_fn: 누락된 동기화: + typedef 시그니처 추가.
- nvmf_stop_listen_ctx.cb_arg: 누락된 값 범위: + 동기화: 추가.

### 완료 (module/bdev/raid/bdev_raid.c — 2026-06-07 확정)

`module/bdev/raid/bdev_raid.c` (6422 라인)에 대해 4섹션 상단 블록 + 모든 함수 §2 헤더
+ 모든 구조체/enum 필드 §4 멀티라인 + #include/매크로/실행 라인 §3 인라인이 갖춰진 것을 검수했다.

파일 전체(lines 1~6422)가 기준을 충족하며, 주요 커버리지는 다음과 같다:

- **파일 상단 블록**: 4섹션(파일의 역할/전체 아키텍처에서의 위치/타 모듈과의 연결/주요 함수·구조체 요약) 모두 3~5문장 이상.
- **구조체/enum §4**: `raid_bdev_io_channel`(process.offset/target_ch/ch_processed 포함 전 필드), `raid_bdev_process_state` 4종 enum, `raid_process_qos`(QoS 토큰 버킷 6필드), `raid_bdev_process`(전 필드), `raid_process_finish_action`, `raid_bdev_examine_others_ctx`, `raid_bdev_examine_ctx`, `raid_bdev_process_base_bdev_remove_ctx`.
- **함수 §2**: 전체 130+ 함수 모두 §2 헤더(호출 체인/파라미터/반환값/실행 컨텍스트). 주요 함수:
  - I/O 경로: `raid_bdev_submit_request`, `raid_bdev_io_complete`, `_raid_bdev_request_split`, `raid_bdev_io_split`, `raid_bdev_submit_rw_request`
  - 채널 관리: `raid_bdev_create_cb`, `raid_bdev_destroy_cb`, `raid_bdev_ch_process_setup/cleanup`
  - 구성/해제: `_raid_bdev_create`, `raid_bdev_configure`, `raid_bdev_deconfigure`, `raid_bdev_free`, `_raid_bdev_destruct`
  - 프로세스(rebuild) 관리: `raid_bdev_process_thread_run`, `raid_bdev_process_thread_init`, `raid_bdev_process_start`, `raid_bdev_process_finish`, `raid_bdev_process_lock_window_range`, `raid_bdev_process_request_complete`
  - examine 경로: `raid_bdev_examine`, `raid_bdev_examine_sb`, `raid_bdev_examine_others`, `raid_bdev_examine_cont`, `raid_bdev_examine_load_sb`, `raid_bdev_create_from_sb`
  - DIF/DIX: `raid_bdev_remap_dix_reftag`, `raid_bdev_verify_dix_reftag`
- **인라인 주석 §3**: 전체 실행 라인(선언·대입·조건·루프·return) 모두 인라인, 원자 연산·크로스스레드 메시지·QoS 토큰 버킷·프로세스 윈도우·DIF reftag 재매핑 등 도메인 특화 설명 포함.
- **전역 변수/매크로**: `g_raid_bdev_list`, `g_raid_modules`, `g_shutdown_started`, `g_opts`, `g_raid_if`, `g_raid_bdev_fn_table`, `RAID_OFFSET_BLOCKS_INVALID`, `RAID_BDEV_PROCESS_MAX_QD` 모두 §3/§4 완비.
- **트레이스 등록**: `SPDK_TRACE_REGISTER_FN(bdev_raid_trace)` — BDEV_RAID_IO_START/DONE tpoint 및 parent↔child 관계 등록 주석 완비.

### 완료 (module/bdev/nvme/ 소형 파일 5개 — 2026-06-07 확정)

`module/bdev/nvme/` 아래 5개 소형 파일에 대해 4섹션 상단 블록 + 모든 함수 §2 헤더
+ 모든 구조체/enum 필드 §4 멀티라인 + #include/매크로/실행 라인 §3 인라인을 완비했다.

- **`bdev_mdns_client.c`** (940 라인): Avahi mDNS 자동 디스커버리 엔진.
  - `mdns_discovery_entry_ctx` 전 필드(name/trid/drv_opts/tailq/ctx) §4 완비 — 설정자/읽는자/값범위/동기화 포함.
  - `mdns_discovery_ctx` 전 필드(name/svcname/hostnqn/sb/poller/drv_opts/bdev_opts/seqno/stop/tailq/mdns_discovery_entry_ctxs) §4 완비.
  - 전역 g_avahi_poll/g_avahi_client §3 인라인. 모든 함수(static 포함) §2 헤더.
  - 인라인: Avahi 이벤트 루프 iterate 패턴, cross-thread app 스레드 메시지 전송(spdk_thread_send_msg), TXT 레코드 파싱, adrfam 결정, 중복 entry 체크 등 전 실행 라인 커버.

- **`bdev_nvme_cuse_rpc.c`** (250 라인): CUSE 가상 /dev/nvmeX RPC.
  - `rpc_nvme_cuse_register.name` + `rpc_nvme_cuse_unregister.name` §4 완비.
  - `rpc_bdev_nvme_cuse_register` / `rpc_bdev_nvme_cuse_unregister` §2 완비(4단계 동작, 호출 체인, 실행 컨텍스트).

- **`nvme_rpc.c`** (769 라인): NVMe passthrough (bdev_nvme_send_cmd) RPC.
  - `rpc_bdev_nvme_send_cmd_req` 전 필드(name/cmd_type/data_direction/timeout_ms/data_len/md_len/cmdbuf/data/md) §4 완비.
  - `rpc_bdev_nvme_send_cmd_resp` 전 필드(cpl_text/data_text/md_text) §4 완비.
  - `rpc_bdev_nvme_send_cmd_ctx` 전 필드(jsonrpc_request/req/resp/nvme_ctrlr/ctrlr_io_ch) §4 완비.
  - 커스텀 디코더 7종(rpc_decode_cmd_type, rpc_decode_data_direction, rpc_decode_cmdbuf, rpc_decode_data, rpc_decode_data_len, rpc_decode_metadata, rpc_decode_metadata_len) §2 완비.
  - base64 ↔ 바이너리 변환, DMA hugepage 할당, SQE 64바이트 검증 등 인라인 전 커버.

- **`vbdev_opal.c`** (985 라인): TCG Opal SED Locking Range vbdev.
  - `opal_vbdev` 전 필드(nvme_ctrlr/opal_dev/bdev_part/locking_range_id/range_start/range_length/opal_base/tailq) §4 완비.
  - `vbdev_opal_bdev_io` 전 필드(ch/bdev_io/bdev_io_wait) §4 완비.
  - `vbdev_opal_channel.part_ch` §4 완비.
  - `vbdev_opal_part_base` 전 필드(nvme_ctrlr_name/part_base/part_tailq/tailq) §4 완비.
  - 전역 g_opal_base TAILQ §4 완비. 모든 함수(static 포함) §2 헤더 + 실행 라인 인라인.

- **`vbdev_opal_rpc.c`** (632 라인): Opal SED 관리 RPC 5종.
  - `rpc_bdev_nvme_opal_init` 전 필드(nvme_ctrlr_name/password) §4 완비.
  - `rpc_bdev_nvme_opal_revert` 전 필드(nvme_ctrlr_name/password — PSID 설명 포함) §4 완비.
  - `rpc_bdev_opal_create` 전 필드(nvme_ctrlr_name/nsid/locking_range_id/range_start/range_length/password) §4 완비.
  - `rpc_bdev_opal_get_info` 전 필드(bdev_name/password) §4 완비.
  - `rpc_bdev_opal_delete` 전 필드(bdev_name/password) §4 완비.
  - `rpc_bdev_opal_set_lock_state` 전 필드(bdev_name/user_id/password/lock_state) §4 완비.
  - `rpc_bdev_opal_new_user` 전 필드(bdev_name/admin_password/user_id/user_password) §4 완비.
  - 모든 RPC 핸들러 §2 헤더 + 실행 라인 인라인.

### 미완료 (우선순위 순)

| 디렉토리 | 설명 | 우선순위 |
|---------|------|---------|
| lib/nvmf/ | NVMe-oF target (subsystem.c·transport.c 완료, tcp.c·ctrlr.c·nvmf_rpc.c 부분 완료) | P1 |
| module/bdev/nvme/ | NVMe bdev 모듈 (소형 5파일 완료, bdev_nvme.c·bdev_nvme_rpc.c·ctrlr.c 등 대형 파일 미완료) | P1 |
| module/bdev/aio/, malloc/, lvol/ | 기타 bdev 모듈 (raid/bdev_raid.c 완료) | P2 |
| lib/env_dpdk/ | DPDK 바인딩 | P2 |
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
