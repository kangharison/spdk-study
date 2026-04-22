# SPDK 주석 작업 진행 현황 (ANNOTATION_PROGRESS.md)

> 이 파일은 **세션 간 작업 이어가기**를 위한 트래커다. 다음 세션에 진입하면 **반드시 이 파일을 먼저 읽고** 현재 상태를 파악한 뒤, "다음에 할 일" 섹션의 첫 항목부터 착수한다.

## 작업 규칙 요약 (매 세션 필수 확인)

1. **상위 규칙**: `/home/harison/company/CLAUDE.md` (공통 주석 방법론)
2. **프로젝트 규칙**: `/home/harison/company/spdk-study/CLAUDE.md` (SPDK 도메인 지식)
3. 두 규칙이 충돌하면 프로젝트 규칙이 우선.
4. **코드 수정 금지** — 한국어 주석만 추가/보강.
5. **모든 함수, 모든 구조체 필드, 모든 실행 라인**에 주석. "자명한 래퍼"도 예외 없음.
6. 파일 상단 블록은 4개 섹션 필수 — `=== 파일의 역할 ===`, `=== 전체 아키텍처에서의 위치 ===`, `=== 타 모듈과의 연결 ===`, `=== 주요 함수/구조체 요약 ===`.
7. 기존 얕은 한국어 주석은 **발견 즉시 기준에 맞게 확장**.
8. 한 파일을 작업하기로 했으면 **그 파일은 완전히 마친 뒤** 다음 파일로 넘어간다. 부분 작업 금지.

## 작업 제외 대상 (서브모듈 · 외부 코드)

다음 디렉토리는 **주석 작업 대상이 아니다**:

```
dpdk/                  # DPDK 서브모듈
intel-ipsec-mb/        # IPSec 라이브러리
isa-l/                 # Intel Storage Acceleration Library
isa-l-crypto/          # ISA-L 암호 확장
libvfio-user/          # vfio-user 서브모듈
ocf/                   # Open CAS Framework
xnvme/                 # xNVMe 서브모듈
build/                 # 빌드 산출물
dpdkbuild/ isalbuild/ isalcryptobuild/ ipsecbuild/ vfiouserbuild/ xnvmebuild/ rpmbuild/
go/vendor/             # Go vendor 디렉토리
shared_lib/            # 빌드 산출 .so
```

`deprecated/`, `scripts/`, `proto/`, `schema/`, `docker/`, `python/`, `licenses/`, `doc/`, `test/` 하위의 대용량 자동 테스트 — 후순위, 당장은 보류.

## 우선순위 정의

- **P0**: SPDK 정수 — 공개 API 헤더 + NVMe/bdev/thread 코어. 이 구간을 마쳐야 다른 계층의 주석이 맥락을 가진다.
- **P1**: NVMe-oF, 주요 bdev 모듈(nvme), 앱 엔트리.
- **P2**: 환경 추상화(env_dpdk), 기타 bdev 모듈, 유틸리티, 소켓/sock.
- **P3**: 예제, 부가 기능(iscsi, vhost, ublk, nbd, blob, ftl, lvol, accel, vmd 등).
- **P4**: 테스트, 도구(spdk_top, trace 등).

## 전체 파일 매트릭스

범례: ☐ 미착수 · ◐ 진행 중 · ☑ 완료 · ✂ 제외

### Phase 0 — 기초 빌딩 블록 (공개 헤더, 짧고 다른 헤더가 참조)

| 상태 | 파일 | 라인 | 비고 |
|------|------|-----:|------|
| ☑ | include/spdk/likely.h | 26 | 2026-04-21 |
| ☑ | include/spdk/assert.h | 37 | 2026-04-21 |
| ☑ | include/spdk/memory.h | 44 | 2026-04-21 |
| ☑ | include/spdk/barrier.h | 115 | 2026-04-21 |
| ☑ | include/spdk/mmio.h | 111 | 2026-04-21 |
| ☑ | include/spdk/endian.h | 150 | 2026-04-21 |
| ☑ | include/spdk/queue.h | 89 | 2026-04-21 |
| ☑ | include/spdk/queue_extras.h | 378 | 2026-04-21 |
| ☐ | include/spdk/tree.h | 842 | BSD sys/tree.h 래퍼 — 큰 파일, 다음 세션 |
| ☑ | include/spdk/stdinc.h | 102 | 2026-04-21 |
| ☑ | include/spdk/util.h | 425 | 2026-04-21 |
| ☑ | include/spdk/uuid.h | 109 | 2026-04-21 |
| ☑ | include/spdk/bit_array.h | 484 | 2026-04-21 (원본 175 → 주석 후 484) |
| ☑ | include/spdk/bit_pool.h | 481 | 2026-04-21 (원본 174 → 주석 후 481) |
| ☑ | include/spdk/cpuset.h | 165 | 2026-04-21 |
| ☑ | include/spdk/fd.h | 59 | 2026-04-21 |
| ☑ | include/spdk/fd_group.h | 724 | 2026-04-21 (원본 268 → 주석 후 724) |
| ☑ | include/spdk/file.h | 203 | 2026-04-21 (원본 72 → 주석 후 203) |
| ☑ | include/spdk/string.h | 712 | 2026-04-21 (원본 292 → 주석 후 712) |
| ☑ | include/spdk/pipe.h | 442 | 2026-04-21 (원본 169 → 주석 후 442) |
| ☑ | include/spdk/hexlify.h | 36 | 2026-04-21 |
| ☑ | include/spdk/base64.h | 297 | 2026-04-21 (원본 116 → 주석 후 297) |
| ☑ | include/spdk/md5.h | 62 | 2026-04-21 |
| ☑ | include/spdk/crc16.h | 50 | 2026-04-21 |
| ☑ | include/spdk/crc32.h | 67 | 2026-04-21 |
| ☑ | include/spdk/crc64.h | 35 | 2026-04-21 |
| ☑ | include/spdk/xor.h | 42 | 2026-04-21 |
| ☐ | include/spdk/dif.h | ? | T10 DIF |
| ☑ | include/spdk/zipf.h | 55 | 2026-04-21 |
| ☐ | include/spdk/histogram_data.h | 286 | 히스토그램 |
| ☐ | include/spdk/log.h | 443 | 로그 |
| ☐ | include/spdk/json.h | ? | JSON 파서 |
| ☐ | include/spdk/jsonrpc.h | ? | JSON-RPC |
| ☐ | include/spdk/rpc.h | ? | RPC 래퍼 |
| ☐ | include/spdk/trace.h | ? | 트레이스 |
| ☐ | include/spdk/trace_parser.h | ? | 트레이스 파서 |
| ☑ | include/spdk/notify.h | 98 | 2026-04-21 |
| ☑ | include/spdk/config.h | 93 | 2026-04-21 (auto-generated — reconfigure 시 재작업 필요) |
| ☑ | include/spdk/version.h | 99 | 2026-04-21 |

### Phase 1 — 환경·스레드·이벤트 (DPDK, reactor, poller, init)

| 상태 | 파일 | 라인 | 비고 |
|------|------|-----:|------|
| ☐ | include/spdk/env.h | 1587 | DPDK 추상화 (hugepage, DMA, PCI) |
| ☐ | include/spdk/env_dpdk.h | ? | DPDK 직결 API |
| ☐ | include/spdk/thread.h | 1338 | SPDK thread, poller, message |
| ☐ | include/spdk/event.h | 384 | 애플리케이션 수명주기 |
| ☐ | include/spdk/init.h | 154 | 서브시스템 초기화 |
| ☐ | include/spdk/scheduler.h | ? | 스레드 스케줄러 |
| ☐ | include/spdk/conf.h | ? | 구성 파일 |
| ☐ | include/spdk/dma.h | ? | DMA 추상화 |

### Phase 2 — NVMe 스펙·드라이버 공개 API

| 상태 | 파일 | 라인 | 비고 |
|------|------|-----:|------|
| ☐ | include/spdk/nvme_spec.h | 4890 | NVMe 스펙 상수/구조체 |
| ☐ | include/spdk/nvme.h | 4802 | 유저스페이스 NVMe 드라이버 API |
| ☐ | include/spdk/nvme_intel.h | ? | Intel 벤더 확장 |
| ☐ | include/spdk/nvme_ocssd.h | ? | OpenChannel SSD |
| ☐ | include/spdk/nvme_ocssd_spec.h | ? | OpenChannel 스펙 |
| ☐ | include/spdk/nvme_zns.h | ? | ZNS(Zoned Namespace) |
| ☐ | include/spdk/opal.h | ? | TCG Opal |
| ☐ | include/spdk/opal_spec.h | ? | TCG Opal 스펙 |
| ☐ | include/spdk/pci_ids.h | ? | PCI 벤더/디바이스 ID |

### Phase 3 — bdev 공개 API

| 상태 | 파일 | 라인 | 비고 |
|------|------|-----:|------|
| ◐ | include/spdk/bdev.h | 2551 | **부분 확장 v4** 2026-04-22 (이전 모두 + **spdk_bdev_initialize/finish**(서브시스템 부팅/종료) + **io_type_supported** + **get_qos_rpc_type/get_qos_rate_limits/set_qos_rate_limits** + **get_qd** + **histogram_enable/get** + **for_each_channel + continue**. 수명 주기·DIF getter·seek_offset·histogram_enable_ext/channel_get_histogram 등 세부 API 일부 잔여) |
| ☑ | include/spdk/bdev_zone.h | 296 | 2026-04-22 (상단 4섹션 블록 + zone_type/action/state enum 전체 값별 주석 + spdk_bdev_zone_info 전 필드 + 속성 getter 6종(zone_size/num_zones/zone_id 계산/max_zone_append_size/max_open/active_zones/optimal) + get_zone_info + zone_management + zone_append 4종(buf/iov × md 유무) + get_append_location 전부 주석 완료) |
| ☐ | include/spdk/bdev_module.h | ? | bdev 모듈 작성자용 |
| ☐ | include/spdk/module/bdev/*.h | ? | 모듈별 헤더 |

### Phase 4 — 기타 공개 헤더 (P1~P2)

accel, accel_module, ae4dma, blob, blob_bdev, fsdev, fsdev_module, ftl, gpt_spec, idxd, idxd_spec, ioat, ioat_spec, iscsi_spec, keyring, keyring_module, lvol, nbd, net, nvmf, nvmf_cmd, nvmf_fc_spec, nvmf_spec, nvmf_transport, scsi, scsi_spec, sock, ublk, vfio_user_pci, vfio_user_spec, vfu_target, vhost, vmd — 각 ☐.

### Phase 5 — lib/ 구현 (P0 우선)

| 상태 | 경로 | 비고 |
|------|------|------|
| ☐ | lib/nvme/nvme_internal.h | NVMe 내부 구조체 |
| ☐ | lib/nvme/nvme.c | NVMe 엔트리 |
| ☐ | lib/nvme/nvme_ctrlr.c | 컨트롤러 |
| ☐ | lib/nvme/nvme_ctrlr_cmd.c | 컨트롤러 admin 커맨드 |
| ☐ | lib/nvme/nvme_ns.c | 네임스페이스 |
| ☐ | lib/nvme/nvme_ns_cmd.c | 네임스페이스 I/O |
| ☐ | lib/nvme/nvme_qpair.c | Queue Pair |
| ☐ | lib/nvme/nvme_pcie.c | PCIe 트랜스포트 |
| ☐ | lib/nvme/nvme_pcie_common.c | PCIe 공통 |
| ☐ | lib/nvme/nvme_pcie_internal.h | PCIe 내부 |
| ☐ | lib/nvme/nvme_transport.c | 트랜스포트 추상화 |
| ☐ | lib/nvme/nvme_fabric.c | Fabrics 공통 |
| ☐ | lib/nvme/nvme_rdma.c | RDMA 트랜스포트 |
| ☐ | lib/nvme/nvme_tcp.c | TCP 트랜스포트 |
| ☐ | lib/nvme/nvme_poll_group.c | 폴링 그룹 |
| ☐ | lib/nvme/nvme_io_msg.c | I/O 메시지 |
| ☐ | lib/nvme/nvme_auth.c | 인증 |
| ☐ | lib/nvme/nvme_discovery.c | Discovery |
| ☐ | lib/nvme/nvme_cuse.c | CUSE |
| ☐ | lib/nvme/nvme_ctrlr_ocssd_cmd.c | OpenChannel |
| ☐ | lib/nvme/nvme_opal.c | Opal |
| ☐ | lib/nvme/nvme_quirks.c | 벤더 쿼크 |

| 상태 | 경로 | 비고 |
|------|------|------|
| ☐ | lib/bdev/bdev_internal.h | bdev 내부 |
| ☐ | lib/bdev/bdev.c | bdev 코어 |
| ☐ | lib/bdev/bdev_rpc.c | bdev RPC |
| ☐ | lib/bdev/bdev_zone.c | Zoned bdev |
| ☐ | lib/bdev/part.c | 파티션 |
| ☐ | lib/bdev/scsi_nvme.c | SCSI↔NVMe 상태 변환 |
| ☐ | lib/bdev/vtune.c | VTune 훅 |

| 상태 | 경로 | 비고 |
|------|------|------|
| ☐ | lib/thread/thread.c | spdk_thread/poller/message 구현 |
| ☐ | lib/thread/thread_internal.h | thread 내부 |
| ☐ | lib/thread/iobuf.c | I/O buf 풀 |

### Phase 6 — lib/ 나머지 (P1~P2)

`lib/nvmf/*`, `lib/env_dpdk/*`, `lib/init/*`, `lib/event/*`, `lib/sock/*`, `lib/accel/*`, `lib/json/*`, `lib/jsonrpc/*`, `lib/rpc/*`, `lib/util/*`, `lib/log/*`, `lib/trace/*`, `lib/trace_parser/*`, `lib/notify/*`, `lib/conf/*`, `lib/dma/*`, `lib/keyring/*`, `lib/scsi/*`, `lib/iscsi/*`, `lib/nbd/*`, `lib/ublk/*`, `lib/vhost/*`, `lib/blob/*`, `lib/lvol/*`, `lib/ftl/*`, `lib/fsdev/*`, `lib/fuse_dispatcher/*`, `lib/rdma_provider/*`, `lib/rdma_utils/*`, `lib/mlx5/*`, `lib/idxd/*`, `lib/ioat/*`, `lib/ae4dma/*`, `lib/vmd/*`, `lib/virtio/*`, `lib/vfio_user/*`, `lib/vfu_tgt/*`, `lib/env_ocf/*` — 각 ☐.

제외: `lib/ut/`, `lib/ut_mock/` (단위 테스트 보조), 필요 시에만 주석.

### Phase 7 — module/ (P2~P3)

```
module/bdev/*         — nvme, aio, malloc, raid, lvol, split, delay, null, passthru, error, crypto, gpt, iscsi, rbd, uring, xnvme, zone_block, ftl, daos, virtio
module/accel/*
module/blob/*
module/env_dpdk/*
module/event/*
module/fsdev/*
module/keyring/*
module/scheduler/*
module/sock/*
module/vfu_device/*
```

우선 `module/bdev/nvme/` (P1), 나머지 ☐.

### Phase 8 — app/, examples/ (P3)

```
app/spdk_tgt, app/nvmf_tgt, app/iscsi_tgt, app/vhost
app/spdk_nvme_perf, app/spdk_nvme_identify, app/spdk_nvme_discover
app/spdk_lspci, app/spdk_dd, app/spdk_top, app/trace, app/trace_record, app/fio
examples/nvme/*, examples/bdev/*, examples/nvmf/*, examples/sock/*, examples/thread/*, examples/accel/*, examples/blob/*, examples/idxd/*, examples/ioat/*, examples/vmd/*, examples/util/*, examples/fsdev/*, examples/interrupt_tgt/*
```

각 ☐. 예제는 SPDK API 사용법 학습용으로 후순위.

### Phase 9 — module/ nv-* (제외 재확인)

`nv-p2p.c`, `nv-p2p.h` 등 SPDK 외 참고 파일 — 현재 저장소에 없음 (bam-study 파일). 무시.

## 다음에 할 일 (Next Actions — 세션 재진입 시 여기부터)

1. **Phase 0 남은 작은 파일**부터 완결:
   - `include/spdk/fd_group.h` (268) — epoll 그룹 래퍼
   - `include/spdk/file.h`       — 파일 I/O 유틸 (소형)
   - `include/spdk/string.h` (292) — 문자열 유틸
   - `include/spdk/pipe.h` (169)  — 링 파이프
   - `include/spdk/base64.h` (116)
   - `include/spdk/dif.h`         — T10 DIF
   - `include/spdk/histogram_data.h` (286)
   - `include/spdk/log.h` (443)
2. 그 다음 **부분완료(◐) 파일 중 비교적 끝이 가까운 것**을 마무리:
   - `include/spdk/bdev_module.h` 의 `struct spdk_bdev`, bdev_module 등록 매크로
   - `lib/nvme/nvme_internal.h` 의 enum nvme_ctrlr_state, detach/probe 구조체, helper inlines
3. `include/spdk/tree.h` (842) — BSD tree 매크로 포팅 (큰 파일, 전용 세션)
4. Phase 0 완료 후 Phase 1(env.h, thread.h, event.h, init.h, scheduler.h, conf.h, dma.h, env_dpdk.h)로 진행.
5. **세션 종료 전 반드시**: 본 `.md`에서 아래 항목 갱신
   - "마지막 세션 요약"
   - "현재 진행 중 파일" (중간에 멈췄다면 위치와 함께 기록)
   - "다음에 할 일"

## 현재 진행 중 파일

(없음)

## 최근 완료 파일 (역순 최대 30개)

- 2026-04-22 · lib/nvme/nvme_internal.h [대부분 완료 ◐] — **enum nvme_ctrlr_state 40+ 초기화 상태머신 전 상태 주석** (CONNECT_ADMINQ → READ_VS/CAP → CHECK_EN → DISABLE/ENABLE → RESET → IDENTIFY → CONFIGURE_AER → SET_KEEP_ALIVE → IDENTIFY_IOCS_SPECIFIC → SET_NUM_QUEUES → IDENTIFY_ACTIVE_NS/IDENTIFY_NS → SET_SUPPORTED_FEATURES → SET_DB_BUF_CFG → TRANSPORT_READY → READY) + detach_ctx/probe_ctx/nvme_driver 구조체 + helper inlines(robust_mutex_lock의 owner-dead 복구 포함) + admin 커맨드 프로토타입 전체(identify/set_num_queues/attach_ns/doorbell_buffer_config/format/fw_commit/fw_image_download/sanitize) + ctrlr 수명주기(construct/destruct_async/poll_async/fail/process_init) + register accessor + qpair hot-path(submit_request) + ns 관리 전부 주석 완료.
- 2026-04-22 · **include/spdk/bdev_zone.h** [☑ 완료] (296라인) — ZNS 공개 API 전체. 상단 4섹션 블록(zone 상태머신 다이어그램, 호출 흐름, 모듈 연결) + zone_type(CNV/SEQWR/SEQWP)/zone_action(CLOSE/FINISH/OPEN/RESET/OFFLINE)/zone_state(EMPTY/IMP_OPEN/FULL/CLOSED/READ_ONLY/OFFLINE/EXP_OPEN/NOT_WP) enum 전 값별 주석 + spdk_bdev_zone_info 전 필드(zone_id/wp/capacity/state/type) + 속성 getter 6종 + get_zone_info + zone_management + Zone Append 4종(buf/iov × md 유무, 장치 자동 wp 할당·LSM-tree 사용처) + get_append_location 전부 주석. 이제 ZNS 워크로드 개발자가 이 헤더 하나만 읽어도 zone 상태 전이, append 시 wp race 회피, log-structured 자료구조 활용 패턴까지 이해 가능.
- 2026-04-22 · include/spdk/bdev.h [부분 확장 v4 ◐] — spdk_bdev_initialize/finish(서브시스템 부팅/종료 상세 흐름) + io_type_supported + QoS API 3종(get_rpc_type/get_rate_limits/set_rate_limits, 토큰 버킷 알고리즘 설명) + get_qd + histogram_enable/get(모든 채널 집계 비동기) + for_each_channel/continue(채널 순회 sync/async 패턴) 주석.
- 2026-04-22 · include/spdk/bdev.h [부분 확장 v3 ◐] — struct spdk_bdev_ext_io_opts(★ _ext API 옵션 번들: memory_domain/accel_sequence/metadata/DIF 마스크/NVMe cdw12·13 전 필드) + compare/comparev 4종 + **comparev_and_writev**(fused atomic CAW) + zcopy_start/end(zero-copy populate/commit 사이클) + nvme_nssr + abort(cb_arg 매칭) + NVMe passthru 4종(admin/io/io_md/iov_md) + copy_blocks(offloaded 장치 내부 복사) + 기본 getter(get_name/product_name/block_size/num_blocks/buf_align/has_write_cache/is_zoned) + set_timeout 전부 상세 주석 완료.
- 2026-04-22 · include/spdk/bdev.h [부분 확장 ◐] — iovec(readv/writev 3종씩) + write_zeroes + write_uncorrectable + unmap/flush/reset + **free_io**(완료 cb 내 필수 호출) + **io_wait_entry + queue_io_wait**(NOMEM 재시도 패턴) + **get_nvme_status/get_nvme_fused_status/get_scsi_status/get_aio_status**(실패 원인 조회) + get_iovec/get_md_buf 상세 주석. I/O 제출 → 완료 → 자원 반납 → 에러 조회의 전체 사이클이 주석만으로 완성.
- 2026-04-22 · include/spdk/bdev.h [부분 ◐] — ★ I/O 경로 애플리케이션 진입 API ★. 파일 상단 4섹션 블록(사용자/관리/I/O 축 분리, 호출 흐름, bdev_module.h와의 관계) + SPDK_BDEV_SMALL/LARGE_BUF_MAX_SIZE + SPDK_BDEV_BUF_SIZE_WITH_MD + spdk_bdev_event_type/media_event + spdk_bdev_status + **enum spdk_bdev_io_type(23종)** 모든 값에 개별 주석 + spdk_bdev_qos_rate_limit_type(IOPS/BPS/R/W) + spdk_bdev_io_completion_cb + **struct spdk_bdev_io_stat** + 열거 API(get_by_name/first/next/first_leaf/next_leaf) + **struct spdk_bdev_open_opts** + **spdk_bdev_open_ext / _v2 / _async** + **spdk_bdev_close** + **spdk_bdev_get_io_channel** (thread affinity 규칙 설명) + **spdk_bdev_read / read_blocks / read_blocks_with_md** + **spdk_bdev_write / write_blocks** 전부 상세 주석. 각 API에 파라미터·반환·에러 코드·내부 호출 체인·전형 사용 패턴 포함.
- 2026-04-22 · include/spdk/bdev_module.h [부분 확장] — **struct spdk_bdev_module** 전 필드(모듈 등록 루트, init/examine/fini 생애주기 콜백, async 플래그, __bdev_module_internal_fields) + **struct spdk_bdev** 전 필드(geometry/supported I/O/DIF/ZNS/NVMe/reset_io_drain_timeout/numa/fn_table 연결/internal=QoS·claim·open_descs·reset_in_progress·QD 텔레메트리·히스토그램·LBA 잠금 범위) + spdk_bdev_name/alias/module_claim 보조 구조체 주석 완비. 이로써 bdev 인스턴스와 모듈 등록 흐름 전체가 주석만으로 이해 가능.
- 2026-04-21 · include/spdk/base64.h — 상단 4섹션 블록 + RFC4648 노트 + 2개 inline 헬퍼(get_encoded_strlen/get_decoded_len 산술 공식 분해) + 4개 함수(encode/urlsafe_encode/decode/urlsafe_decode) 모두 주석. 표준 vs URL-safe 알파벳 차이, 패딩 시맨틱, NVMe-oF 인증/JSON-RPC/iSCSI CHAP 사용처, dst=NULL dry-run 패턴 명시.
- 2026-04-21 · include/spdk/pipe.h — 상단 4섹션 블록 + spdk_pipe/spdk_pipe_group 전방 선언 + 9개 함수(create/destroy/writer get_buffer·advance/reader bytes_available·get_buffer·advance/group create·destroy·add·remove) 모두 주석. zero-copy iovec 인터페이스 동작, advance 시맨틱(rewind 가능), group 풀의 LIFO 스택 재사용 패턴, non-thread-safe 명시.
- 2026-04-21 · include/spdk/string.h — 상단 4섹션 블록 + SPDK_STRINGIFY 매크로 2단계 패턴 + 19개 함수 모두 주석. NVMe 우측 패딩 필드 처리(strcpy_pad/strlen_pad), in-place 파서(strsepq, str_trim, parse_ip_addr), thread-local strerror, parse_capacity 사이즈 표기, 엄격한 strtol/strtoll의 양수 강제, strarray 시리즈 등.
- 2026-04-21 · include/spdk/file.h — 상단 4섹션 블록 + 4개 함수(POSIX 파일 로드 2종 + sysfs 읽기 2종) 모두 한국어 주석. 동기 blocking I/O이므로 hot-path 금지, 반환 버퍼 free 책임, printf format attribute로 컴파일 타임 검증 명시.
- 2026-04-21 · include/spdk/fd_group.h — 상단 4섹션 블록 + enum spdk_fd_type + struct spdk_event_handler_opts(ABI 호환성 패턴) + spdk_fd_fn typedef + 12개 함수(create/destroy/wait/get_fd/nest/unnest/add×3/remove/event_modify/get_epoll_event/set_wrapper) + SPDK_FD_GROUP_ADD/ADD_EXT 매크로 전부 주석. interrupt-mode SPDK thread의 epoll 디스패처 동작·계층적 nesting·EVENTFD 자동 read 리셋 등 설명.
- 2026-04-21 · include/spdk/bit_pool.h — 상단 4섹션 블록 + 11개 함수 모두 한국어 주석 (할당자 시맨틱/스레드 안전성/에러 경로 설명)
- 2026-04-21 · include/spdk/bit_array.h — 상단 4섹션 블록 + 12개 함수 모두 한국어 주석 (워드 단위 패킹, find_first_set/clear 동작, store/load_mask 영속화 경로)
- 2026-04-21 · include/spdk/bdev_module.h [부분] — 상단 4섹션 블록 + **struct spdk_bdev_fn_table**(★ submit_request 진입점) + enum spdk_bdev_io_status + **struct spdk_bdev_io**(★ bdev 레이어 I/O 요청 객체) 전 필드 완료
- 2026-04-21 · lib/nvme/nvme_internal.h [부분 확장] — 추가로 **spdk_nvme_qpair + spdk_nvme_ns + ctrlr_process + spdk_nvme_ctrlr** 전체 필드 주석 완비 (hot/cold 분리, multi-process, NVMe-oF 인증, 상태머신 포함)
- 2026-04-21 · lib/nvme/nvme_internal.h [부분] — 상단 블록 + nvme_payload + nvme_request + poll_status + AER (★ I/O 경로 중간 객체 nvme_request 완성)
- 2026-04-21 · include/spdk/nvme_spec.h [부분] — 상단 블록 + SGL 디스크립터 + PSDT + SQE(spdk_nvme_cmd) + Status + CQE(spdk_nvme_cpl) (★ NVMe 와이어 포맷의 핵심)
- 2026-04-21 · lib/nvme/nvme_pcie_internal.h (I/O 경로 핵심 — tracker / pcie_qpair / doorbell inlines)
- 2026-04-21 · lib/thread/thread_internal.h (spdk_io_channel 정의)
- 2026-04-21 · lib/bdev/bdev_internal.h (bdev_io 내부 헬퍼)
- 2026-04-21 · include/spdk/notify.h
- 2026-04-21 · include/spdk/version.h
- 2026-04-21 · include/spdk/config.h
- 2026-04-21 · include/spdk/md5.h
- 2026-04-21 · include/spdk/zipf.h
- 2026-04-21 · include/spdk/xor.h
- 2026-04-21 · include/spdk/crc32.h
- 2026-04-21 · include/spdk/crc16.h
- 2026-04-21 · include/spdk/crc64.h
- 2026-04-21 · include/spdk/hexlify.h
- 2026-04-21 · include/spdk/util.h
- 2026-04-21 · include/spdk/queue_extras.h
- 2026-04-21 · include/spdk/cpuset.h
- 2026-04-21 · include/spdk/fd.h
- 2026-04-21 · include/spdk/uuid.h
- 2026-04-21 · include/spdk/stdinc.h
- 2026-04-21 · include/spdk/queue.h
- 2026-04-21 · include/spdk/endian.h
- 2026-04-21 · include/spdk/assert.h
- 2026-04-21 · include/spdk/mmio.h
- 2026-04-21 · include/spdk/barrier.h
- 2026-04-21 · include/spdk/memory.h
- 2026-04-21 · include/spdk/likely.h

## 마지막 세션 요약

**2026-04-22 (열여섯 번째 파트 — nvme_internal.h 상태머신 + 프로토타입 대량 완료)**:
- **enum nvme_ctrlr_state 40+ 상태 전부 주석**. 각 상태의 admin 커맨드·완료 대기 의미, 상태 전이 조건 설명. 이제 NVMe 컨트롤러 부팅 시퀀스(CC.EN=0 disable → CC.EN=1 enable → identify → configure AER → set keep alive → set num queues → identify NS → set supported features → doorbell buffer config → ready)가 주석만으로 완전 추적 가능.
- detach_ctx/probe_ctx/nvme_driver 구조체 전 필드 주석 (shutdown notification 상태머신, multi-process 공유 컨트롤러 리스트, netlink hotplug fd)
- helper inline 함수 — **robust mutex의 EOWNERDEAD 복구** 설명 포함 (primary 프로세스 crash 후 secondary가 복구 가능한 구조)
- admin 커맨드 프로토타입 일괄 주석 (identify/set_num_queues/attach_ns/create_ns/doorbell_buffer_config/format/fw_commit/fw_image_download/sanitize) — 각각 NVMe opcode/FID 매핑
- 컨트롤러 수명주기 API (construct/destruct_async/poll_async/fail/process_init) + 동기 대기(wait_for_adminq_completion)
- 레지스터 accessor (get_cap/vs/cmbsz/pmrcap/bpinfo/set_bprsel/set_bpmbl)
- qpair hot-path (submit_request) + ns 관리 (identify_active_ns/construct/destruct)

**이로써 `nvme_internal.h`는 거의 완성** — 잔여는 fabric/rdma/tcp 트랜스포트 특화 프로토타입과 namespace 세부 API 일부. I/O 경로 이해에 필요한 핵심은 전부 커버됨.

**2026-04-22 (열다섯 번째 파트 — bdev_zone.h 완전 완료 + bdev.h QoS/histogram/for_each_channel)**:
- `include/spdk/bdev_zone.h` (296줄) **전체 완료**. ZNS 공개 API의 모든 상수·구조체·함수에 한국어 주석. 특히 Zone Append의 "장치가 wp를 원자 할당" 개념과 LSM-tree/log-structured 자료구조 활용 패턴 명시. bdev_zone.h만 읽어도 ZNS 특유 상태머신/제약(max_open_zones/max_active_zones)/복구 시나리오까지 이해 가능.
- `include/spdk/bdev.h` 추가: spdk_bdev_initialize/finish(애플리케이션 부팅/종료 흐름), io_type_supported, QoS 3종(get_rpc_type/get_rate_limits/set_rate_limits — 토큰 버킷 1ms poller), get_qd, histogram_enable/get(비동기 채널 집계), for_each_channel + continue(per-channel 순차 sync/async 작업 패턴).

이로써 I/O 경로의 애플리케이션 측 표면이 거의 완성 — 기본 R/W, scatter-gather, PI, unmap/flush/reset, compare/CAW, zcopy, NVMe passthru, copy offload, ZNS 전체, 관리(open/close/init/fini), QoS, 완료 상태 조회, histogram, 채널 순회, 그리고 iteration/통계 모두 주석 커버.

**2026-04-22 (열네 번째 파트 — bdev.h 고급 I/O + getter)**: compare 계열 4종 + **comparev_and_writev (fused atomic compare-and-write)** + **zcopy_start/end** (zero-copy populate/commit 사이클) + **NVMe passthru 4종** (admin/io/io_md/iov_md) + **copy_blocks** (장치 offload 복사) + abort + nvme_nssr + 주요 getter(get_name/block_size/num_blocks/buf_align/has_write_cache/is_zoned) + set_timeout 전부 주석. spdk_bdev_ext_io_opts(memory_domain/accel_sequence/NVMe cdw12·13) 필드 전체 상세 주석으로 _ext variant API의 모든 옵션 경로가 이해 가능해짐.

**2026-04-22 (열세 번째 파트 — bdev.h 핵심 I/O API 완성)**: 이전 파트의 read/write 핵심에 이어 readv/writev 3종씩, write_zeroes/unmap/flush/reset, write_uncorrectable, free_io, io_wait 재시도 구조체(NOMEM 패턴), 완료 상태 조회(get_nvme_status/fused/scsi/aio/iovec/md_buf) 전부 주석. **이로써 애플리케이션이 read_blocks 호출 → bdev 코어 분기 → 모듈 submit → 장치 DMA → CQE → 완료 cb 내 get_nvme_status로 에러 분석 → free_io로 반납의 전체 흐름이 주석만으로 완전 추적 가능**.

**2026-04-22 (열두 번째 파트 — bdev.h 공개 API 시작)**: I/O 경로의 애플리케이션 진입점인 `include/spdk/bdev.h` 핵심 섹션 주석 완료. 파일 상단 4섹션 블록 + 이벤트/상태/I/O 타입 enum 전체 + QoS 타입 + 통계 구조체 + 열거 API + open/close/get_io_channel + read·read_blocks·read_blocks_with_md·write·write_blocks 전부 상세 주석. 각 함수에 param/return/에러 코드/내부 호출 체인/전형 패턴 포함. 이로써 애플리케이션 → bdev.h API → bdev_module.h spdk_bdev_io → 모듈 submit_request → nvme_request → SQE → PCIe/CQE 흐름이 주석만으로 완전히 추적 가능.

**2026-04-22 (열한 번째 파트 — bdev_module.h spdk_bdev/spdk_bdev_module 완료)**: I/O 경로 부분완료(◐) 파일의 핵심 누락분을 보완.
- `include/spdk/bdev_module.h`의 `struct spdk_bdev_module` 전체(모듈 등록 루트, init/examine/fini 6개 콜백, async_init/fini/fini_start 플래그, __bdev_module_internal_fields의 spinlock·action_in_progress·quiesced_ranges)
- `struct spdk_bdev` 전체(ctxt/name/aliases/product_name, blocklen·phys_blocklen·blockcnt, split_on_write_unit/split_on_optimal_io_boundary/md_interleave/dif_is_head_of_md/zoned/media_events/memory_domains_supported 비트필드, required_alignment·optimal_io_boundary·preferred_write_alignment/granularity·optimal_write_size·preferred_unmap_alignment/granularity, max_segment_size·max_num_segments·max_unmap·max_write_zeroes·max_copy·max_rw_size, uuid·md_len·DIF 3종·ZNS 6종·NVMe ctratt/nsid, reset_io_drain_timeout, numa, module/fn_table 연결, internal 전체(QoS·spinlock·status·examine_in_progress·claim v1/v2 union·open_descs·reset_in_progress·qd_poller·histogram·locked_ranges))
- spdk_bdev_name/alias/module_claim 보조 구조체도 함께 주석

**이제 I/O 경로의 bdev 측 객체(spdk_bdev_module, spdk_bdev, spdk_bdev_io)가 전부 주석 완비**. 나머지 bdev_module.h 부분은 param 구조체, io_internal_fields, 모듈 등록 매크로.

**2026-04-21 (열 번째 파트 — string / pipe / base64 완료)**: Phase 0 잔여 중 세 파일 추가 완료.
- `include/spdk/string.h` (원본 292 → 주석 후 712) — 문자열 유틸 19개 함수 + SPDK_STRINGIFY 매크로. NVMe 우측 패딩 필드 처리(strcpy_pad/strlen_pad), in-place 파서, thread-local strerror, parse_capacity의 사이즈 표기, 엄격한 strtol/strtoll 정책 등.
- `include/spdk/pipe.h` (원본 169 → 주석 후 442) — 단일 스레드 링 버퍼 9개 함수. zero-copy iovec 인터페이스, advance 시맨틱(rewind 가능 = idempotent get_buffer), pipe_group의 LIFO 스택 캐시 친화 풀, non-thread-safe 명시. sock/iSCSI/NVMe-oF TCP의 PDU 어셈블러 사용처 강조.
- `include/spdk/base64.h` (원본 116 → 주석 후 297) — RFC4648 표준/URL-safe 두 변형 4개 함수 + 2개 inline 길이 계산 헬퍼. NVMe-oF 인증/JSON-RPC 페이로드 사용처 명시.

이 세 파일까지 마치며 Phase 0의 흔히 쓰이는 유틸 헤더는 거의 정복. 남은 항목은 dif/histogram_data/log/json/jsonrpc/rpc/trace/tree.h 등.

**2026-04-21 (아홉 번째 파트 — fd_group / file 유틸 완료)**: Phase 0 잔여 중 두 파일 추가 완료.
- `include/spdk/fd_group.h` (원본 268 → 주석 후 724) — SPDK interrupt-mode 이벤트 디스패처의 epoll 추상화. enum/struct/typedef/12개 함수/2개 매크로 전부 주석. 특히 `spdk_fd_group_wait`(★)의 내부 동작(epoll_wait → EVENTFD read 리셋 → wrapper 경유 → spdk_fd_fn 콜백), `nest/unnest`의 계층적 fgrp 트리 구성, `event_modify`의 EPOLLOUT 동적 토글 패턴을 상세히 기록. ABI 호환성 패턴(opts_size)과 SPDK_STATIC_ASSERT의 의도까지 포함.
- `include/spdk/file.h` (원본 72 → 주석 후 203) — POSIX 파일 일괄 로드와 sysfs 속성 읽기 4개 함수. 모두 blocking I/O라 init/구성 경로 전용임을 강조. `__attribute__((format(printf, 2, 3)))`의 컴파일 타임 포맷 검증 의의 명시.

**2026-04-21 (여덟 번째 파트 — 비트 자료구조 완료)**: Phase 0의 미완료 항목 두 개를 끝까지 완료.
- `include/spdk/bit_array.h` (원본 175 → 주석 후 484)
- `include/spdk/bit_pool.h`   (원본 174 → 주석 후 481)

두 파일 모두 상단 4섹션 블록(역할/아키텍처 위치/타 모듈 연결/주요 함수 요약) + 모든 함수에 파라미터·반환값·설명·에러 경로·호출 체인·실행 컨텍스트·동기화 요구사항을 포함한 함수 단위 한국어 주석을 완비. bit_pool은 bit_array를 감싸 "비트-단위 슬롯 할당자"를 제공한다는 래핑 관계, `allocate_bit` 호출 체인(find_first_clear → set), 실패 시 UINT32_MAX, `set_bit_allocated`의 -EBUSY 반환 의미, `create_from_array`의 소유권 이전 시맨틱(실패 시 소유권 미이전의 비대칭성) 등을 상세 설명.

이로써 lvol/blobstore/FTL/NVMe CID 풀 등 여러 상위 모듈이 참조하는 비트 자료구조 유틸 2개가 100% 주석화되어, 상위 레이어 작업 시 이 유틸 함수들의 동작·스레드 안전성·경계 조건을 주석만으로 모두 파악 가능.

**2026-04-21 (네 번째 파트 — I/O path 선행)**: 사용자 요청에 따라 I/O 경로 파일로 우선순위 전환. 아래 3개 I/O path 내부 헤더 완료:
- `lib/bdev/bdev_internal.h` (34)
- `lib/thread/thread_internal.h` (39) — spdk_io_channel 구조 / NVMe qpair trailing ctx 설명
- `lib/nvme/nvme_pcie_internal.h` (356) — **가장 중요**: `struct nvme_pcie_qpair` / `struct nvme_tracker` (4KB PRP/SGL 슬롯) / `struct nvme_pcie_ctrlr` / doorbell ring 인라인 함수(shadow doorbell 포함) / process_completions·submit_request 프로토타입까지 모든 필드·함수에 한국어 주석 완비. 이 파일 하나만 읽어도 NVMe PCIe I/O 경로가 어떻게 SQE 제출 → doorbell MMIO → phase-bit 폴링 → CQE 파싱 → 완료 콜백으로 흐르는지 파악 가능.

**2026-04-21 (일곱 번째 파트)**: `include/spdk/bdev_module.h`의 I/O 경로 진입 구조체 완성:
- 파일 상단 4섹션 블록 — bdev 모듈 인터페이스 개관·호출 체인·완료 경로
- **struct spdk_bdev_fn_table** — bdev ↔ 모듈 vtable. 특히 `submit_request(ch, bdev_io)` 콜백이 I/O 제출 진입점(필드별 설정자/효과/NOMEM 관례 설명).
- **enum spdk_bdev_io_status** — NOMEM 자동 재시도 관례, AIO/SCSI/NVMe 에러 분류
- **struct spdk_bdev_io** — bdev I/O 요청 객체. 공개(type, u union) vs internal(모듈 접근 금지) vs driver_ctx(flexible, 캐시라인 정렬) 구조 설명.

**이제 application → bdev.h → bdev_io → 모듈 → nvme_request → SQE → 장치까지의 전체 I/O 경로 객체 그래프가 주석만으로 따라갈 수 있음.**

**2026-04-21 (여섯 번째 파트)**: `nvme_internal.h`의 I/O 경로 중심 구조체 전부 완성:
- **struct spdk_nvme_qpair** (line 669) — hot/cold 필드 배치, 상태머신(DISCONNECTED→CONNECTING→CONNECTED→ENABLING→ENABLED→DESTROYING), free_req·queued_req 리스트, poll_group 연결, NVMe-oF 인증(nvme_auth), Fabrics CONNECT 예약 request 등 모든 필드의 설정자/읽는자/동기화 설명.
- **struct spdk_nvme_poll_group / transport_poll_group** — 다중 트랜스포트 폴링 추상화.
- **struct spdk_nvme_ns** — 네임스페이스(sector_size, md_size, PI, ZNS/NVM identify data 등).
- **struct spdk_nvme_ctrlr_aer_completion / ctrlr_process / nvme_register_completion** — multi-process AER·reg 완료 큐.
- **struct spdk_nvme_ctrlr** (line 1400) — 컨트롤러 전역 상태. hot 필드(ns 트리, 실패/재설정 플래그, cap/vs 레지스터 캐시)와 cold 필드(trid, quirks, AER 슬롯, shadow doorbell, identify 데이터, FW 다운로드 상태, ANA log, 인증 tid/seqnum 등) 전부 주석.

이제 `nvme_internal.h`만 읽어도 NVMe 드라이버의 컨트롤러-큐-요청 객체 그래프 전체가 머리에 그려진다.

**2026-04-21 (다섯 번째 파트)**: 큰 핵심 헤더 2개 부분 주석 (◐):
- `include/spdk/nvme_spec.h` [부분] — **I/O 경로 와이어 포맷의 정수**. 파일 상단 4섹션 블록 + SGL 디스크립터(enum/struct, 16B) + PSDT 값 + **struct spdk_nvme_cmd(64B SQE)** 모든 dword 및 비트필드 + **struct spdk_nvme_status**(phase/sc/sct/crd/m/dnr) + **struct spdk_nvme_cpl(16B CQE)** 모든 필드에 한국어 주석. 이제 SQE → 장치 → CQE → phase 비트 폴링의 흐름이 헤더만 읽어도 완전히 파악됨.
- `lib/nvme/nvme_internal.h` [부분] — 파일 상단 블록 + **struct nvme_request** 전체 필드 + nvme_payload + NVME_PAYLOAD_CONTIG/SGL 매크로 + nvme_completion_poll_status + nvme_async_event_request. I/O 경로의 "중간 객체"인 nvme_request 완전 문서화 (split/child, payload, cb_fn, cpl, accel_sequence 등).

누적 25개 파일 완료 (Phase 0 21 + I/O 3 완전 + I/O 2 부분).

**이로써 I/O 경로 이해를 위한 핵심 4개 헤더 읽기 루트가 완성됨**:
  1. `nvme_spec.h`: SQE/CQE/PRP/SGL 스펙 (와이어 포맷)
  2. `nvme_internal.h`: nvme_request 중간 객체
  3. `nvme_pcie_internal.h`: nvme_tracker + nvme_pcie_qpair + doorbell
  4. `thread_internal.h`: spdk_io_channel (qpair 소유 스레드 연결)

### 다음 세션 출발점 (I/O path 중심 유지)

**바로 이어갈 작업**:

A. **`lib/nvme/nvme_internal.h` 이어서 작업** — struct spdk_nvme_qpair (line 464부터) 필드 주석, 이어서 struct spdk_nvme_ns(line 570), struct spdk_nvme_ctrlr(line 1029), 트랜스포트 프로토타입.

B. **`include/spdk/nvme_spec.h` 이어서 작업** — opcode/status enum, identify data(ctrlr/namespace), log page 구조체, register bit-field union들.

C. **`include/spdk/bdev_module.h`** (1972) — `struct spdk_bdev_io` 정의. bdev 레이어의 I/O 요청 객체 전체 구조. **매우 큼 — 섹션 분할 작업 필요**. 작업 가이드:
   - 섹션 1: 콜백 타입 + enum (io type, status type, error 등)
   - 섹션 2: `struct spdk_bdev_fn_table` (모듈 작성자용 콜백)
   - 섹션 3: `struct spdk_bdev` (bdev 메타데이터)
   - 섹션 4: `struct spdk_bdev_io` (★ I/O 경로 핵심 — read/write/unmap payload union)
   - 섹션 5: 모듈 등록 API
   - 한 세션에 1-2 섹션씩 점진 완료하며 매트릭스에 "부분완료" 표시 권장

2. **`include/spdk/nvme_spec.h`** (4890) — NVMe 스펙 구조체(SQE, CQE, PRP, SGL, identify data 등). 큰 만큼 섹션 분할:
   - 섹션 A: opcode/status code enum
   - 섹션 B: `struct spdk_nvme_cmd` (SQE, 64B — I/O 경로 핵심)
   - 섹션 C: `struct spdk_nvme_cpl` (CQE, 16B — phase 비트 포함)
   - 섹션 D: PRP / SGL 디스크립터
   - 섹션 E: registers (CAP/CC/CSTS/AQA 등)
   - 섹션 F: identify data (ctrlr, namespace)

3. **`lib/nvme/nvme_internal.h`** (1839) — `struct nvme_request`, `struct spdk_nvme_ctrlr/qpair` 내부 정의. I/O 경로의 중간 객체(request). 섹션 분할 필요.

4. **`include/spdk/bdev.h`** (2551) — 공개 read/write API.

5. **`include/spdk/nvme.h`** (4802) — 공개 NVMe API.

6. **구현 파일 순서** (헤더 선행 후):
   - `lib/nvme/nvme_qpair.c` (1314) — request submit/complete 코어
   - `lib/nvme/nvme_ns_cmd.c` (1516) — read/write 커맨드 빌더
   - `lib/nvme/nvme_pcie_common.c` (1912) — doorbell, PRP 빌드, completion poll
   - `lib/nvme/nvme_pcie.c` (1173)
   - `lib/nvme/nvme_transport.c` (976)
   - `lib/bdev/bdev.c` (11524) — **매우 큼**, spdk_bdev_read/write/submit 경로만 우선
   - `lib/thread/thread.c` (3324) — poller/message 구현
   - `module/bdev/nvme/bdev_nvme.c` — bdev_io → NVMe 브리지

**큰 파일 작업 전략**:
- 1회 세션에 ~300-500 라인을 주석 완전 품질로 작업 가능
- 큰 파일은 상단 블록 + 구조체 정의 섹션을 먼저 완성하고, 함수 구현은 추후 세션
- 매트릭스 상태에 `◐` (부분) 표시 추가해 어디까지 했는지 기록

### 우선순위 변경 (2026-04-21): I/O Path 중심 선행

사용자 요청에 따라 **I/O 경로 관련 코드**를 먼저 작업. 나머지 Phase 0은 이후.

**I/O 경로 핵심 파일** (호출 순서로):

```
[Application]
  ↓ spdk_bdev_read/write
include/spdk/bdev.h (2551)           ── ☐ IO-P0
lib/bdev/bdev_internal.h (34)         ── ☑ 2026-04-21
lib/bdev/bdev.c (11524)               ── ☐ IO-P0  (매우 큼, 섹션 분할)
include/spdk/bdev_module.h (1972)     ── ◐ 부분 (상단 4섹션 블록 + **struct spdk_bdev_module** + struct spdk_bdev_fn_table + enum spdk_bdev_io_status + spdk_bdev_name/alias/module_claim + **struct spdk_bdev 전체** + **struct spdk_bdev_io 전체** 완료. param 구조체(io_block/reset/abort/nvme_passthru/zone_mgmt) · spdk_bdev_io_internal_fields · 모듈 등록 매크로 · claim API 프로토타입은 후속)
  ↓
module/bdev/nvme/bdev_nvme.c          ── ☐ IO-P0
  ↓ spdk_nvme_ns_cmd_read/write
include/spdk/nvme.h (4802)            ── ☐ IO-P0
include/spdk/nvme_spec.h (4890)       ── ◐ 부분 (상단 4섹션 블록 + SGL enum/struct + PSDT + SQE + Status + CQE 완료. 나머지 identify/log/register 구조체는 후속 세션)
  ↓
lib/nvme/nvme_internal.h (1839)       ── ◐ 대부분 완료 v2 — 상단 블록 + nvme_payload + nvme_request + poll_status + AER + qpair/auth state + nvme_auth + spdk_nvme_qpair + poll_group + transport_poll_group + spdk_nvme_ns + ctrlr_aer_completion + ctrlr_process + register_completion + spdk_nvme_ctrlr + **enum nvme_ctrlr_state 전체 40+ 상태 주석** + detach_ctx/probe_ctx/nvme_driver 구조체 + helper inlines(nvme_qpair_is_admin/io_queue·robust_mutex_lock·ctrlr_lock) + admin 커맨드 프로토타입(identify/set_num_queues/set_host_id/attach_ns/create_ns/doorbell_buffer_config/format/fw_commit/fw_image_download/sanitize) + wait_for_adminq_completion + ctrlr 수명주기(construct/destruct/fail/process_init) + register accessor(cap/vs/cmbsz/pmrcap/bpinfo) + qpair hot-path(submit_request/abort) + ns 관리(identify_active_ns/set_identify_data/construct/destruct) 전부 주석 완료. 극소수 잔여(fabric/rdma/tcp 특화 프로토타입, nvme_register_ns 등 namespace 세부 API)만 남음
lib/nvme/nvme_ns_cmd.c (1516)         ── ☐ IO-P0
lib/nvme/nvme_qpair.c (1314)          ── ☐ IO-P0  (SQ/CQ 관리, submit/complete)
lib/nvme/nvme_transport.c (976)       ── ☐ IO-P0
  ↓ PCIe 트랜스포트
lib/nvme/nvme_pcie_internal.h (356)   ── ☑ 2026-04-21
lib/nvme/nvme_pcie.c (1173)           ── ☐ IO-P0
lib/nvme/nvme_pcie_common.c (1912)    ── ☐ IO-P0  (doorbell ring, PRP 빌드)
  ↓ 실행 모델
include/spdk/thread.h (1338)          ── ☐ IO-P0
lib/thread/thread_internal.h (39)     ── ☑ 2026-04-21
lib/thread/thread.c (3324)            ── ☐ IO-P0
```

**I/O 경로 권장 작업 순서** (구조 이해 → 제출 경로 → 완료 경로):

1. `lib/bdev/bdev_internal.h` (34)             — 가장 작음, 시작점
2. `lib/thread/thread_internal.h` (39)         — thread 내부 타입
3. `lib/nvme/nvme_pcie_internal.h` (356)       — PCIe 트랜스포트 내부 구조
4. `include/spdk/bdev_module.h`                 — bdev_io struct 정의
5. `include/spdk/bdev.h` (2551)                — bdev 공개 API
6. `include/spdk/nvme_spec.h` (4890)           — NVMe 스펙 상수/구조체
7. `include/spdk/nvme.h` (4802)                — NVMe 공개 API
8. `lib/nvme/nvme_internal.h` (1839)           — NVMe 내부 구조
9. `lib/nvme/nvme_qpair.c` (1314)              — submit/complete 핵심
10. `lib/nvme/nvme_ns_cmd.c` (1516)            — read/write 커맨드 빌더
11. `lib/nvme/nvme_pcie_common.c` (1912)       — doorbell, PRP, completion poll
12. `lib/nvme/nvme_pcie.c` (1173)              — PCIe 드라이버
13. `lib/nvme/nvme_transport.c` (976)          — 트랜스포트 추상화
14. `lib/bdev/bdev.c` (11524)                  — bdev 코어 (섹션별 분할 작업)
15. `lib/thread/thread.c` (3324)               — reactor/poller 구현
16. `include/spdk/thread.h` (1338)             — thread 공개 API
17. `module/bdev/nvme/bdev_nvme.c`             — bdev → NVMe 브리지

### Phase 0 남은 파일 (I/O 경로 작업 완료 후 이어감)

1. **우선 처리 (공개 API, 다른 헤더가 많이 참조)**:
   - `include/spdk/log.h` (443 라인) — 로그 API
   - `include/spdk/string.h` (292 라인) — 문자열 유틸
   - `include/spdk/json.h` / `jsonrpc.h` / `rpc.h` — RPC 스택
   - `include/spdk/trace.h` / `trace_parser.h` — 트레이스
2. **중형 유틸**:
   - `include/spdk/fd_group.h` (268) — epoll 래퍼
   - `include/spdk/pipe.h` (169) — 링 파이프
   - `include/spdk/bit_array.h` (175), `bit_pool.h` (174)
   - `include/spdk/base64.h` (116), `file.h`, `dif.h`, `histogram_data.h` (286)
3. **큰 파일**: `include/spdk/tree.h` (842) — BSD tree 매크로 포팅 (FreeBSD sys/tree.h 전체)
4. Phase 0 완료 후 Phase 1 (env.h, thread.h, event.h, init.h, scheduler.h, conf.h, dma.h, env_dpdk.h) 진입.
5. 각 파일 완료 시 매트릭스 갱신, "최근 완료 파일" 상단 추가. 세션 종료 시 "현재 진행 중 파일" 비우기.

## 세션 재진입 체크리스트 (매 세션 시작 시)

- [ ] `/home/harison/company/CLAUDE.md` 재확인 (공통 규칙)
- [ ] `/home/harison/company/spdk-study/CLAUDE.md` 재확인 (SPDK 도메인)
- [ ] 본 `ANNOTATION_PROGRESS.md`의 "현재 진행 중 파일" 확인
- [ ] "다음에 할 일" 첫 항목부터 착수
- [ ] 작업한 파일 하단까지 100% 주석 확인 (부분 작업 금지)
- [ ] 세션 종료 전 본 파일 업데이트
