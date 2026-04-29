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
| ☑ | include/spdk/dif.h | 999 | 2026-04-28 **완료** (병렬 agent) — 원본 492 → 999줄. T10 DIF/DIX 보호정보 처리 완비. 22개 함수 + 9개 매크로 + 3개 enum + 3개 구조체 (dif_ctx 14필드 등) 모두. Guard CRC + AppTag + RefTag, NVMe PI Type 1/2/3 + 16/32/64bit format, DIF(인터리브) vs DIX(분리) 두 모드 + stream API. |
| ☑ | include/spdk/zipf.h | 55 | 2026-04-21 |
| ☑ | include/spdk/histogram_data.h | 609 | 2026-04-28 **완료** (병렬 agent) — 원본 286 → 609줄. 11개 함수 + 7개 매크로(GRANULARITY/BUCKET 등) + 1개 구조체(4필드) + typedef. logarithmic range × linear bucket 패턴, bdev I/O latency 분포 → P50/P99 산출, per-thread + merge로 lockless 집계. |
| ☑ | include/spdk/log.h | 886 | 2026-04-28 **확인** (병렬 agent) — 이미 완료 상태 (원본 443 → 주석 후 886줄). 25개 함수 + 15개 매크로(SPDK_NOTICELOG/WARNLOG/ERRLOG/PRINTF/INFOLOG/DEBUGLOG/LOGDUMP/REGISTER_COMPONENT/DEPRECATION_*) + 6값 enum + 2개 구조체. syslog+stderr 이중 sink, 컴포넌트별 디버그 플래그 자동 등록(constructor) + deprecation 추적. |
| ☑ | include/spdk/json.h | 843 | 2026-04-29 **완료** (병렬 agent) — 원본 353 → 843줄. 매크로 7 + enum 11값 + 구조체 2 + 함수 ~60개 + typedef 2. zero-copy 파서 + streaming writer. RPC/config 파일 control-plane 핵심. |
| ☑ | include/spdk/jsonrpc.h | 715 | 2026-04-29 **완료** (병렬 agent) — 원본 357 → 715줄. 에러 코드 6 + 불투명 타입 5 + 구조체 client_response + typedef 3 + 함수 21. JSON-RPC 2.0 server/client. rpc.py ↔ /var/tmp/spdk.sock 표준 트랜스포트. |
| ☑ | include/spdk/rpc.h | 363 | 2026-04-29 **완료** (병렬 agent) — 원본 156 → 363줄. state 매크로 2 + 자동 등록 매크로 2 + typedef + 함수 9. SPDK 자체 RPC system. SPDK_RPC_REGISTER constructor 자동 등록 + STARTUP/RUNTIME phase gating + UDS listen. |
| ☑ | include/spdk/trace.h | 1153 | 2026-04-29 **완료** (병렬 agent) — 원본 506 → 1153줄. 매크로 14 + 구조체 9 + 함수 21. lockless circular buffer 기반 lightweight logging. SPDK_TRACE_REGISTER_FN constructor 자동 등록, hot path → per-lcore history → /dev/shm. SPDK 거의 모든 라이브러리에서 사용. |
| ☑ | include/spdk/trace_parser.h | 314 | 2026-04-29 **완료** (병렬 agent) — 원본 131 → 314줄. enum 1(2값) + 구조체 3 + 함수 6. trace.h binary entry → 사람용 변환. lcore merge-sort, object lifecycle/cross-reference 자동 결합. C++ 구현(lib/trace_parser/). |
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
| ☑ | include/spdk/dma.h | 901 | 2026-04-29 **완료** (병렬 agent) — 원본 472 → 901줄. 매크로 1 + enum 5값 + 콜백 typedef 7 + 구조체 5 + 함수 18. DMA memory domain 추상화 — bdev I/O 시 메모리 위치(host RAM/GPU/RDMA registered/CMB) 표현 + translate로 zero-copy I/O. accel_sequence와 결합. |

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
| ☑ | lib/nvme/nvme.c | 2026-04-28 **완료** — 원본 2277줄 → 주석 후 2914줄. 기존 일부 주석 위에 빠진 함수 30+개 보강. 파일 상단 4섹션 블록(드라이버 단일 인스턴스 g_spdk_nvme_driver 다중 프로세스 hugepage 공유, probe/attach/detach 진입점, admin command 동기 polling 헬퍼, ref counting via robust mutex) + nvme_ctrlr_shared(PCIe만 공유 가능) + nvme_ctrlr_connected/detach_async_finish/detach_async/detach_poll_async + ★ spdk_nvme_detach 동기 wrapper + ★ spdk_nvme_detach_async/poll_async/poll(다중 ctrlr 컨테이너 패턴, FIFO 보존 INSERT_HEAD 트릭) + ★ nvme_completion_poll_cb(timed_out 자동 free + cpl 복사 + done=true) + dummy_disconnected_qpair_cb(no-op placeholder) + ★ nvme_wait_for_completion_poll(admin lock, poll_group vs qpair 분기, PCIe CSTS all-ones link 검사, timed_out 마킹) + nvme_wait_for_adminq_completion(180s timeout 변환, release 옵션) + ★ nvme_user_copy_cmd_complete(CONTROLLER_TO_HOST 결과 복사, PID 검증) + nvme_allocate_request_user_copy(4KiB align DMA 버퍼 + host_to_controller 시 즉시 복사) + ★ nvme_request_check_timeout(admin/AER/KEEP_ALIVE 분기, multi-process PID 검사) + ★ nvme_robust_mutex_init_shared(PROCESS_SHARED + ROBUST → PI futex 기반 crash-safe mutex) + ★★ nvme_driver_init(g_init_mutex 진입 직렬화, primary memzone_reserve 또는 secondary lookup + 180s polling, hotplug netlink fd, default UUID 생성) + ★ nvme_ctrlr_probe(probe_cb → 기존 ctrlr 검색 → ref 증가 + attach_cb / 신규 construct + init_ctrlrs 추가) + ★ nvme_ctrlr_poll_internal(process_init 폴링 → 실패 destruct_async 누적 / READY → attached 이동 + ref + attach_cb) + nvme_init_controllers(busy-wait wrapper) + nvme_get_ctrlr_by_trid/_unsafe(local + shared 양 리스트 순회) + ★ nvme_probe_internal(trstring 자동 채움, transport_ctrlr_scan, secondary+PCIe 자동 attach 경로 — shared_attached_ctrlrs 순회 + opts/process 검증 + lock unlock-during-cb 패턴) + nvme_dummy_attach_fail_cb(legacy 호환 SPDK_ERRLOG) + nvme_probe_ctx_init(콜백 4종 + 빈 리스트) + ★ spdk_nvme_probe/probe_ext + spdk_nvme_probe_async/_ext(direct_connect=false enumerate) + nvme_connect_probe_cb(opts 강제 적용) + ★ nvme_ctrlr_opts_init(FIELD_OK + SET_FIELD/SET_FIELD_ARRAY ABI 호환 매크로) + ★ spdk_nvme_connect/connect_async(direct_connect=true) + spdk_nvme_trid_populate_transport(trtype→trstring 표준 매핑 5종) + spdk_nvme_transport_id_populate_trstring(toupper 정규화, GCC-11 LTO false positive 회피) + parse/str 4쌍(trtype/adrfam) + ★ parse_next_key(key:val/key=val 파서, ':'와 '='의 우선순위) + ★ spdk_nvme_transport_id_parse(인식/무시 키 분류) + spdk_nvme_host_id_parse(같은 문자열 두 번 파싱 패턴) + cmp_int + ★ spdk_nvme_transport_id_compare(trtype 우선 + PCIe BDF 정규화 + Fabrics 4필드 순차) + spdk_nvme_prchk_flags_parse/str(reftag/guard 4 조합) + spdk_nvme_scan_attached(빈 probe_ctx + 트랜스포트 scan_attached 위임) + ★ nvme_parse_addr(getaddrinfo 래퍼, gai 코드 음수 정규화) + ★ nvme_get_default_hostnqn(UUID NQN 표준 형식 "nqn.2014-08.org.nvmexpress:uuid:...") + SPDK_LOG_REGISTER_COMPONENT(nvme) 전부 상세 주석. **결과**: probe/attach/detach 전 진입 경로, multi-process hugepage 공유 driver 객체 라이프사이클, admin completion 동기 polling 패턴, user_copy 헬퍼, robust mutex의 PI futex crash-safe 메커니즘, ABI 호환 SET_FIELD 매크로 패턴, transport_id 파싱/비교의 PCIe vs Fabrics 분기가 모두 주석만으로 추적 가능. |
| ◐ | lib/nvme/nvme_ctrlr.c | 2026-04-28 **부분 완료 v2** — Part 2: qpair 관리 섹션 10종 추가 (총 6510줄). v1 핵심 8종 + qpair 10종 = 18종 보강. v2에서 추가: spdk_nvme_ctrlr_get_opts(opts 포인터 노출), nvme_ctrlr_proc_add_io_qpair(★ multi-process active_procs 등록), ★ spdk_nvme_ctrlr_get_default_io_qpair_opts(13개 필드 기본값 + ABI 호환 SET_FIELD), nvme_ctrlr_io_qpair_opts_copy(SPDK_STATIC_ASSERT 80B 가드), ★ nvme_ctrlr_create_io_qpair(qprio/AMS=RR 검증, qid 할당, transport create, active_io_qpairs 등록), ★★ spdk_nvme_ctrlr_alloc_io_qpair(★ 핵심 사용자 API — state==READY 검증, sq/cq buffer_size 검증, interrupt+delay_cmd_submit 충돌, create+connect+8단계 cleanup), ★ spdk_nvme_ctrlr_reconnect_io_qpair(상태 4분기 -ENODEV/-EAGAIN/-ENXIO/0), spdk_nvme_ctrlr_get_admin_qp_failure_reason, nvme_ctrlr_disconnect_qpair(lock 자동 wrapper), ★★ spdk_nvme_ctrlr_free_io_qpair(★ 7단계 — in_completion_context 자기 free 패턴 + DISCONNECTING 폴링 + DESTROYING 마킹 + foreign qpair 안전성 검사 + 4단계 cleanup). v1 변경 없음(state_string/set_state 트리오/process_init/construct/destruct_async/free_*data). 잔여: opts 헬퍼 큰 함수 spdk_nvme_ctrlr_get_default_ctrlr_opts (84줄), Features 단계, 실패/리셋, Configure/IDs, AER, multi-process, process_init sub-callback 9종, public APIs 50+개. 다음 세션 계속 (Part 3에서 features/reset 우선). | — 원본 5997줄 → 주석 후 6265줄. **기존 일부 주석 위에 핵심 함수 8종 보강**. 보강된 함수: nvme_ctrlr_state_string(★ 상태머신 40+ 상태 문자열 매핑 + 그룹 분류 다이어그램 — INIT/DISABLE/ENABLE/IDENTIFY/NS DISCOVERY/FEATURES/최종, "WAIT_FOR_*" 패턴 의미), _nvme_ctrlr_set_state(KEEP_EXISTING vs INFINITE vs ms 변환 + overflow 방어), nvme_ctrlr_set_state/quiet 짝(quiet=같은 상태 반복 진입 시 로그 폭주 방지), nvme_ctrlr_free_zns/iocs_specific_data + free_doorbell_buffer(NVMe 1.3+ shadow doorbell), ★★★ nvme_ctrlr_process_init(★ controller bring-up 상태머신 driver — 호출 컨텍스트 + 동작 패턴 3 stage + 정상 경로 시퀀스 다이어그램 INIT→READY 30+ 상태 + Reset/Error 분기 + 호출자), nvme_robust_mutex_init_recursive_shared(RECURSIVE + ROBUST + PSHARED 3종 속성, vs init_shared 비교), ★ nvme_ctrlr_construct(7단계 — INIT_DELAY vs INIT 분기, admin_queue_size 검증/정규화 max/quirk multiple/min, 플래그 0 클리어, 빈 컨테이너 초기화, ctrlr_lock 초기화), nvme_ctrlr_destruct_finish/destruct_async(★ destruct 비동기 시퀀스 — is_destructed 마킹으로 새 attach 거부, queued aborts/AER 취소, IO qpair 강제 정리, doorbell/IOCS data free, shutdown_async 시작). **잔여 작업** (상당량): get_default_ctrlr_opts 등 옵션 헬퍼, alloc/free/connect/reconnect_io_qpair 등 qpair 관리, set_intel_log_pages/ANA log/supported_features/host_feature 등 features 단계, fail/shutdown_async/poll/enable, disable/disconnect/reset/reconnect, set_num_queues/keep_alive/host_id, AER 처리(async_event_cb, configure_aer 등), multi-process(get_process/add/remove/cleanup, proc_get/put_ref), process_init의 sub-callback 9종(vs_done/cap_done/check_en/set_en_0/wait_for_ready_0/1 등), keep_alive, public APIs (get_data/regs_csts/cc/cap/vs/cmbsz/pmrcap/bpinfo, get_pmrsz/num_ns/is_active_ns/get_first_active_ns/next_active_ns/get_ns, get_pci_device/numa_id/id/max_xfer_size/max_sges, register_aer_callback/timeout_callback, attach_ns/detach_ns/create_ns/delete_ns/format/update_firmware, reserve_cmb/map_cmb/enable_pmr/map_pmr, boot_partition_start/poll/write, security_receive/send, get_flags/transport_id/alloc_qid/free_qid/get_memory_domains/authenticate). 이 파일은 SPDK NVMe 드라이버에서 가장 큰 단일 파일이라 다세션 분할 필요 — 다음 세션에서 계속. |
| ☑ | lib/nvme/nvme_ctrlr_cmd.c | 2026-04-28 **완료** (병렬 agent) — 원본 1048줄 → 주석 후 1682줄. 28개 admin 커맨드 빌더 함수 모두 + abort 보조 7종 + 4섹션 블록. 각 함수의 NVMe spec opcode/CDW10-15 비트필드 매핑 상세. 그룹: io_cmd_raw 시리즈, identify (CNS/CNTID/CSI), attach/detach/create/delete_ns, doorbell_buffer_config, format (LBAF/MS/PI/PIL/SES), set/get_feature[_ns], set/get_num_queues, set_async_event_config, set_host_id, get_log_page (NUMDL/NUMDU/LPOL/LPOU/LID), abort fan-out (parent/child + ACL), fw_commit/fw_image_download, security_send/receive (SECP/SPSP), sanitize (SANACT/AUSE/OWPASS), directive. lock→allocate→fill→submit 패턴 명시. |
| ☑ | lib/nvme/nvme_ns.c | 2026-04-28 **확인** (병렬 agent) — 1582줄 (이미 이전 세션에 완료된 상태). 24개 함수 모두 표준 양식 준수 + 4섹션 블록 모두 존재. nvme_ns_construct 호출 체인, Identify NS/ID Descriptor/IOCS-specific 3단계 admin 시퀀스, ZNS/NVM CSI 분기, ELBAS PI Format, NOIOB stripe 계산, NGUID/UUID/CSI 디스크립터 파싱, hugepage spdk_zmalloc DMA 할당, AER NS Attribute Notice 재호출 경로 모두 인라인 설명. |
| ☑ | lib/nvme/nvme_ns_cmd.c | 2026-04-24 **완료** — 원본 1516줄 → 주석 후 2592줄. 모든 공개 API 주석 완료: compare 4종(+with_md, +v, +v_with_md), read/write 6종(+with_md, +v, +v_with_md, +_ext, +v_ext), 내부 _ext 빌더 2종(rw_ext, rwv_ext), zone append 3종(check_zone_append + append_with_md + appendv_with_md), 관리형 I/O 6종(write_zeroes, verify, write_uncorrectable, dataset_management/DSM, copy/SCC, flush), reservation 4종(register/release/acquire/report), io_mgmt 2종(recv/send). 핵심(setup_request, _nvme_ns_cmd_rw, spdk_nvme_ns_cmd_read/write)은 이전 세션에 이미 완료. |
| ☑ | lib/nvme/nvme_qpair.c | 2026-04-24 **완료** — 원본 1314줄 → 주석 후 2371줄. 파일 상단 4섹션 블록(제출/완료 호출 그래프, state machine 다이어그램) + 전역 opcode/status 사전 11종(admin/fabric/feat/io/sgl_type/sgl_subtype/status_type/generic/cmd_specific/media_error/path) + 디버그 프린터 11개(nvme_get_sgl_*, nvme_get_prp_string, nvme_get_dptr_string, nvme_get_admin/io_qpair_command_string, nvme_admin/io_qpair_print_command, spdk_nvme_print_command/completion, spdk_nvme_qpair_print_command/completion) + nvme_get_string 선형 검색 + spdk_nvme_cpl_get_status_string/_type_string + nvme_qpair_state_string(수명주기 다이어그램) + nvme_completion_is_retry(DNR 기반 재시도 판정) + **★ nvme_qpair_manual_complete_request**(트랜스포트 경유 없이 가짜 CQE 합성) + **★ abort_queued_reqs / _complete_abort_queued_reqs / abort_queued_reqs_with_cbarg**(SWAP-기반 재귀 회피 + cb_arg 격리) + **★ nvme_qpair_check_enabled**(상태머신 전이 + PCIe reset abort + queued_req flush) + **★ nvme_qpair_resubmit_requests**(완료 개수만큼 flow control 재제출) + **★ nvme_complete_register_operations**(multi-process register 완료 큐) + **★ spdk_nvme_qpair_process_completions**(완료 폴링 진입점, 7단계 상세) + getter 6종(get_fd/failure_reason/abort_dnr/is_connected/get_id/num_outstanding) + **★ nvme_qpair_init**(req_buf 풀 배치, 64B align, reserved_req 특수 슬롯) + **★ nvme_qpair_complete_error_reqs / nvme_qpair_deinit** + **★ _nvme_qpair_submit_request**(9단계 제출 코어: state check → split 재귀 → err injection → submit_tick → ENABLED/FABRIC-CONNECTING 허용 → 트랜스포트 delegate → EAGAIN/error 경로) + **nvme_qpair_submit_request / resubmit_request / abort_all_queued_reqs** + **spdk_nvme_qpair_add/remove_cmd_error_injection** 전부 상세 주석. 이 파일만 읽어도 SPDK NVMe의 submit→doorbell→CQE→callback 완전 경로와 reset/split/err-injection/multi-process 특수 케이스 모두 파악 가능. |
| ☑ | lib/nvme/nvme_pcie.c | 2026-04-25 **완료** — 원본 1173줄 → 주석 후 1920줄. 파일 상단 4섹션 블록(PCIe cold-path 전체 개관, 5개 주요 책임: probe/BAR/MMIO/SIGBUS/CMB-PMR, probe→attach 호출 체인) + 전역 (g_signal_lock, g_sigset, g_hotplug_filter_cb, nvme_pcie_enum_ctx) + **★ nvme_sigbus_fault_sighandler**(PCIe link loss SIGBUS 방어 — atomic CAS로 재진입 방지, BAR을 anonymous 메모리로 MAP_FIXED remap, 0xFF로 채움 → all-ones 감지 경로) + _nvme_pcie_event_process(UEVENT ADD/REMOVE) + _nvme_pcie_hotplug_monitor + reg_addr + get_registers + **★ set/get_reg_4/8**(TLS 마커 g_thread_mmio_ctrlr + spdk_mmio_* + all-ones 감지) + ASQ/ACQ/AQA/CMBLOC/CMBSZ/PMRCAP/PMRCTL/PMRSTS/PMRMSCL/PMRMSCU register wrappers + get_max_xfer_size(NVME_MAX_PRP_LIST_ENTRIES × page_size) + get_max_sges + **★ map_cmb**(CAP.CMBS 확인 + CMBSZ SZU/SZ 해석 + CMBLOC BIR/OFST + BAR mmap + 경계 검증) + unmap_cmb + reserve_cmb + **map_io_cmb**(WDS/RDS 검사 + 4MiB 최소 + 2MB 정렬 + spdk_mem_register) + unmap_io_cmb + **★ map_pmr**(PMRCAP BIR 검사 + BAR mmap + CMSS 지원 시 PMRMSCU/PMRMSCL CBA 설정 + PMRSTS.CBAI 확인) + unmap_pmr + **★ config_pmr**(PMRCTL.EN 토글 + PMRSTS.NRDY 대기 + PMRCAP.PMRTO/PMRTU 기반 timeout) + enable/disable_pmr + map_io_pmr/unmap_io_pmr + **★★ allocate_bars**(spdk_pci_device_map_bar BAR0 + regs 설정 + doorbell_base 계산 + CMB/PMR 매핑) + free_bars + **★★ pcie_nvme_enum_cb**(primary/secondary 분기 + traddr 필터 + nvme_ctrlr_probe 진입) + scan_attached + **★ ctrlr_scan**(hotplug 이벤트 우선 처리 + spdk_pci_enumerate vs device_attach 분기) + **★★ ctrlr_construct**(pci_claim + pctrlr 할당 + opts/quirks/NUMA 복사 + allocate_bars + PCI CMD 0x404 busmaster+INTx disable + CAP read + doorbell_stride 계산 + admin qpair 생성 + process 등록 + SIGBUS 핸들러 최초 등록) + **★ ctrlr_enable**(ASQ/ACQ/AQA 레지스터 세팅 — CC.EN=1 이전 preparation) + ctrlr_destruct + enable_interrupts(VFIO MSI-X) + qpair_iterate_requests + spdk_nvme_pcie_set_hotplug_filter + **★ nvme_pci_driver_id**(SPDK_PCI_CLASS_NVME 매칭 테이블) + **SPDK_PCI_DRIVER_REGISTER**(NEED_MAPPING + WC_ACTIVATE — Write Combining으로 doorbell 성능 향상) + **★★★ pcie_ops**(vtable 전체 — 6 수명주기 + 5 register + 2 limits + 3 CMB + 4 PMR + 4 qpair 수명주기 + 7 qpair hot-path + 10 poll group 포함) + **★★★ SPDK_NVME_TRANSPORT_REGISTER**(constructor로 main() 전에 TAILQ에 등록) 전부 주석. **이 파일 완료로 SPDK NVMe 드라이버의 PCIe 경로 전체(probe → BAR mmap → admin 큐 → CC.EN=1 → I/O hot-path → hotplug/SIGBUS 방어 → 정리)가 주석만으로 완결**. |
| ☑ | lib/nvme/nvme_pcie_common.c | 2026-04-24 **완료** — 원본 1912줄 → 주석 후 3313줄. 파일 상단 4섹션 블록(제출/완료 호출 체인 그래프, nvme_pcie_qpair/tracker 자료구조 맵, hot-path 전체 순서) + 헬퍼 5종(vtophys PCIe/VFIO-USER 분기, qpair_reset phase bit 초기화, qpair_get_fd interrupt-mode, construct_tracker, alloc_cmb bump allocator) + **★ qpair_construct**(SQ/CQ hugepage 할당, tracker 풀 64B align, shadow doorbell 설정, max_completions_cap 계산) + admin_qpair_construct(SHARE hugepage) + multi-process admin 3종(insert/complete_pending_admin + cmd_create/delete_io_cq/sq CDW10-11 설정) + connect 콜백 체인(create_sq_cb with shadow doorbell setup, create_cq_cb) + _create_io_qpair(poll_group shared stats vs 개별 calloc) + connect/disconnect_qpair + **★★ copy_command_mmio**(QEMU 8B-at-a-time) + **★★★ copy_command**(SSE2 non-temporal 128b×4 store) + **★★★★ submit_tracker**(SQ[sq_tail]=SQE + sq_tail wrap + doorbell MMIO write) + **★★★ complete_tracker**(retry 판정 + multi-process insert + nvme_complete_request + free_tr 반환) + manual_complete + abort_trackers(last 저장 무한루프 회피) + admin_abort_aers(AER 전용) + check_timeout(FIFO 순서 가정 조기 break) + **★★★★ process_completions** (7단계: state check → CONNECTING admin 대리 폴링 → ctrlr_lock → max_cap 적용 → phase bit 루프 + next prefetch + memory barrier(PPC/RISC-V/aarch64) + tracker 복원 + complete_tracker → CQ doorbell ring + delay_cmd_submit flush + timeout + admin pending 처리 + vtophys failure 지연 처리) + qpair_destroy/create_io_qpair/delete_io_qpair(2단계 Delete SQ→CQ + shadow doorbell 클리어) + fail_request_bad_vtophys(in_completion_context 분기) + **★★★ prp_list_append** (PRP1/PRP2/PRP list 3가지 모드, 4KB 페이지 경계 엄격 정렬) + build_req 분기 테이블(4 조합 dispatcher) + **★ build_contig_request**(CONTIG→PRP, payload_offset 반영) + **★★ build_contig_hw_sgl_request**(CONTIG→SGL, vtophys mapping_length 기반 물리 segment 분해, 단일/다중 descriptor 분기, ubsan NULL 회피 이중 cast) + **★★ build_hw_sgl_request**(SGL→SGL, next_sge_fn 반복, Bit Bucket SGL 처리, ★ SGL merge 최적화 - 인접 물리 주소면 previous length 확장, 단일 DATA_BLOCK inline vs LAST_SEGMENT 분기) + **★★ build_prps_sgl_request**(SGL→PRP, prp_list_append 누적 호출, 중간 SGE 페이지 경계 assert 방어 검증) + **★ build_metadata**(3 경로: SGL_MPTR_SGL vs CONTIG MPTR 물리주소) + **★★★★ submit_request** (5단계: admin lock → tracker pop → req 연결 + cid = tr->cid → PSDT=PRP 기본 → build_req_fn 분기 → build_metadata → submit_tracker) + poll_group 10종(PCIe는 comp_channel 없이 순차 순회 + g_dummy_stat 리다이렉트) + **SPDK_TRACE_REGISTER_FN**(SUBMIT/COMPLETE 추적점) 모든 함수 본문 inline까지 완료. **이로써 애플리케이션 spdk_nvme_ns_cmd_read() → SQ에 SQE 기록 → doorbell MMIO → 장치 fetch → CQE 기록 → phase bit polling → cb_fn 실행의 전 여정이 주석만으로 추적 가능**. |
| ☑ | lib/nvme/nvme_pcie_internal.h | 2026-04-28 **확인** (병렬 agent) — 712줄, 이미 완료 상태. 4섹션 블록 + nvme_pcie_ctrlr/qpair/tracker/poll_group 전 필드 + 5개 인라인 함수(nvme_pcie_qpair, ctrlr, need_event, update_mmio_required, ring_sq/cq_doorbell) + 모든 프로토타입에 한국어 주석 완비. 124개 한국어 주석 라인 확인. |
| ☑ | lib/nvme/nvme_transport.c | 2026-04-24 **완료** — 원본 976줄 → 주석 후 1820줄. 파일 상단 4섹션 블록(vtable 디스패치 패턴 원리, qpair->transport 캐시 vs nvme_get_transport 조회 경로 분기 근거=multi-process, hot path 호출 체인) + 레지스트리 5종(get_first/next/get + available/available_by_name + transport_register with assert-based dup/overflow 방어) + 컨트롤러 수명주기 7종(construct/scan/scan_attached/destruct/enable/enable_interrupts/ready) + 레지스터 동기 4종(set/get_reg_4/8) + **★ register_operation_completion 헬퍼**(sync op + 가짜 완료 큐잉 패턴, multi-process hugepage 할당) + 비동기 4종(set/get_reg_4/8_async, async 미구현 트랜스포트용 sync fallback 패턴) + 컨트롤러 속성 2종(get_max_xfer_size/get_max_sges) + **CMB 3종**(reserve/map/unmap, Controller Memory Buffer의 PCIe 전용 특성) + **PMR 4종**(enable/disable/map/unmap, Persistent Memory Region, -ENOSYS vs -ENOTSUP 반환 관례 차이) + I/O qpair 관리(create_io_qpair-qpair->transport 캐시 저장, delete_io_qpair-multi-process lookup 이유, connect_qpair_fail, **★ connect_qpair**-동기/비동기 busy-wait 상태머신, disconnect_qpair-idempotent, qpair_get_fd-interrupt mode, **disconnect_qpair_done**-active_proc 기반 abort, get_memory_domains, process_transport_events-ctrlr_lock) + **★★ qpair hot-path 5종 공통 패턴**(spdk_likely!is_admin → qpair->transport 직접 호출, else → nvme_get_transport 재조회) 모두 주석: abort_reqs/reset/**★★★ submit_request**/**★★★ process_completions**/iterate_requests + qpair_authenticate + admin_qpair_abort_aers + **Poll Group 10종**(tgroup 수명주기 + connected/disconnected STAILQ 이동 불변식 + num_connected_qpairs 카운터 유지 + process_completions/check_disconnected 폴링 디스패치 + poll_group_disconnect/connect_qpair의 리스트 이동 로직 + stats 2종) + get_trtype + **★ get/set_opts ABI 호환 SET_FIELD 매크로 패턴** + get_registers(volatile MMIO 포인터 노출 경고) 전부 상세 주석. 이 파일만 읽어도 NVMe 드라이버의 vtable 아키텍처, multi-process safety, hot/cold 경계, optional op 규약 전체 파악 가능. |
| ☑ | lib/nvme/nvme_fabric.c | 2026-04-26 **완료** — 원본 671줄 → 주석 후 1578줄. 파일 상단 4섹션 블록(NVMe-oF의 5대 책임: Property R/W·Discovery·CONNECT·AUTH 진입 판단·수명주기 정리, 호출 체인 3종) + struct nvme_fabric_prop_ctx 전 필드(value/size/cb_fn/cb_arg, 수명주기) + Property Set/Get 헬퍼 6종(prop_set_cmd/prop_set_cmd_sync/prop_set_cmd_done/prop_set_cmd_async + prop_get_cmd/prop_get_cmd_sync/prop_get_cmd_done/prop_get_cmd_async — opcode=0x7F + fctype=0x00/0x04, value union, attrib.size, robust=true/false 차이, 트램펄린 콜백 패턴) + 공개 reg R/W 8종(set/get_reg_4/8 sync/async — 트랜스포트 ops 진입점) + Discovery 3종(discover_probe — subtype/trtype/NQN/traddr/trsvcid 정규화 + nvme_ctrlr_probe 재귀, get_discovery_log_page — GET LOG PAGE LID=0x70, ctrlr_discover — 4KB 페이징 with 헤더 처리·entries[0] 오프셋·numrec 추출·recfmt 검증) + ctrlr_scan(직접 NQN vs DISCOVERY_NQN 분기, discovery_ctrlr 임시 생성→process_init→Identify→destruct, direct_connect 모드는 discovery_ctrlr 자체를 attach) + **★★ qpair_connect_async**(NVMe-oF CONNECT 핸드셰이크 9단계 — 인자 검증·DMA 1024B nvmf_data 할당·status calloc·SQE 빌드(opcode/fctype/qid/sqsize/kato)·**reserved_req 사용 이유**·nvmf_data 페이로드 채우기(admin=cntlid 0xFFFF / IO=ctrlr->cntlid + hostid 16B + hostnqn + subnqn)·submit_request·timeout_tsc 계산·fabric_poll_status 보존) + **★ qpair_connect_poll**(완료 폴링 + cntlid 추출 + atr/ascr 플래그 추출, 응답 status_code_specific.success vs invalid union, sct/sc 에러 로깅) + qpair_poll_cleanup(timed_out 분기로 use-after-free 회피) + qpair_auth_cleanup(idempotent cb 호출) + **★ qpair_auth_required**(4-OR: atr/ascr/dhchap_ctrlr_key/auth.cb_fn) + qpair_connect 동기 래퍼(busy-wait 루프). **이 파일만 읽어도 NVMe-oF의 와이어 포맷(Property Set/Get·Discovery Log Page·CONNECT 페이로드 1024B), 호스트 ↔ 타깃 핸드셰이크 전 시퀀스, 인증 진입 결정 정책, multi-process 안전성 패턴이 주석만으로 완전 추적 가능**. |
| ☐ | lib/nvme/nvme_rdma.c | RDMA 트랜스포트 |
| ☑ | lib/nvme/nvme_tcp.c | 2026-04-29 **완료** (병렬 agent) — 원본 3416줄 → 주석 후 4813줄 (+1397). 50개 함수 보강 + 모든 구조체 필드 + 4섹션 블록. ★ NVMe over TCP (NVMe-oF 1.0 + TP 8000). 카테고리: 헬퍼 5/req 풀 4/PDU 송신 10/PDU 수신 상태머신 10/handshake 5/R2T flow 2/TermReq 3/qpair·ctrlr 라이프사이클 8/TLS PSK 1/vtable 기타 10/poll group 10/trace 3. PDU 종류별 인코딩(CapsuleCmd/Resp, H2C/C2HData, R2T, ICReq/Resp), HDGST/DDGST CRC32C accel vs SW fallback, in-capsule data 결정(ioccsz), TLS PSK HKDF 유도, recv 상태머신(CH→PSH→PAYLOAD→QUIESCING). |
| ☑ | lib/nvme/nvme_poll_group.c | 2026-04-28 **완료** — 원본 515줄 → 주석 후 1248줄. 파일 상단 4섹션 블록(poll group의 4가지 핵심 가치: 트랜스포트 이종 통합 / accel 오프로드 / interrupt 모드 epoll 통합 / disconnected qpair 통지, connect/완료 호출 체인 그래프, 단일 spdk_thread affinity 근거) + spdk_nvme_poll_group_create(★ ABI 호환 SET_FIELD 매크로, accel 콜백 일관성 2단계 검증 — finish/reverse/abort XOR + append→finish 의존성, fd_group lazy 생성, 비-Linux fall-through) + get_fd_group(외부 epoll nest용) + set_interrupt_callback(EEXIST 정책) + ★ Linux 전용 eventfd 트리오: read_disconnect_qpair_fd(epoll 콜백) + write_disconnect_qpair_fd(트랜스포트가 disconnect 통지) + add_disconnect_qpair_fd(eventfd EFD_NONBLOCK|EFD_CLOEXEC + SPDK_FD_GROUP_ADD_EXT 등록 + 단일 호출 assert) + 비-Linux stub + ★★ spdk_nvme_poll_group_add(상태 검증 → enable_interrupts_is_valid first-time 결정 + 이후 일관성 강제 → STAILQ tgroup 검색 → 없으면 nvme_get_first/next_transport로 dlopen된 트랜스포트까지 lazy-create + back pointer + INSERT_TAIL → nvme_transport_poll_group_add 위임) + spdk_nvme_poll_group_remove(disconnected 검증 + tgroup 검색 + 위임) + nvme_qpair_process_completion_wrapper(fd_group 콜백, 0=무제한 폴링) + nvme_poll_group_add/remove_qpair_fd(SPDK_SIZEOF ABI 호환 opts, fd_type 자동 read 위임) + ★ nvme_poll_group_connect_qpair(트랜스포트 connect → fd 등록 → 실패 시 disconnect 롤백 패턴) + nvme_poll_group_disconnect_qpair(fd 제거 우선 → 트랜스포트 disconnect 순서 중요성) + spdk_nvme_poll_group_wait(disconnected_qpairs 선행 통지 → spdk_fd_group_wait timeout=-1 무한 블로킹) + ★★★ spdk_nvme_poll_group_process_completions(★ 메인 폴링 진입점, in_process_completions 재귀 가드, error_reason 첫 음수 보존 + num_completions 양수 누적, spdk_unlikely로 hot-path 분기 최적화, 호출 체인 6 레이어 그래프) + spdk_nvme_poll_group_all_connected(disconnected 즉시 -EIO + CONNECTED 미만 -EIO + CONNECTING -EAGAIN + tgroup early break 최적화) + get_ctx + ★ spdk_nvme_poll_group_destroy(STAILQ_FOREACH_SAFE + REMOVE 후 destroy 실패 시 INSERT_TAIL 롤백 + EBUSY, fd_group disconnect_qpair_fd 먼저 remove 후 close 후 destroy 순서) + ★ get_stats(2단계 순회: 1차 카운트 + 2차 수집, 트랜스포트별 부분 실패 허용 reported_stats_count, 모두 실패 시 -ENOTSUP) + free_stats(trtype 매칭 + freed_stats == num_transports assert 검증) 전부 상세 주석. **결과**: poll group이 reactor 1코어=1 인스턴스 + N 트랜스포트 sub-group + N qpair를 단일 process_completions로 묶는 메커니즘, polling vs interrupt 모드 fd_group 통합 경로, dlopen된 트랜스포트의 lazy tgroup 생성, ABI 호환을 위한 SPDK_SIZEOF/SET_FIELD 패턴, disconnected qpair 통지 4가지 경로(즉시 wait 진입 시 / 정기 process_completions / interrupt eventfd / 사용자 callback)가 모두 주석만으로 추적 가능. |
| ☑ | lib/nvme/nvme_io_msg.c | 2026-04-28 **완료** (병렬 agent) — 원본 217줄 → 주석 후 752줄. 7개 함수(send/process/is_producer_registered/ctrlr_register/update/detach/unregister) + 4섹션 블록. ★ 외부 non-SPDK 스레드가 SPDK NVMe controller에 admin/IO 작업 위탁하는 메시지 채널 — MP-SC ring + 전용 io_qpair + producer STAILQ. SPDK lockless/affinity 원칙의 우회 경로(opal/nvmf 등 producer 모듈이 사용). |
| ☑ | lib/nvme/nvme_auth.c | 2026-04-28 **완료** — 원본 1296줄 → 주석 후 2397줄. 파일 상단 4섹션 블록(NVMe-oF DH-HMAC-CHAP의 5대 가치: PSK 비공개·DH forward secrecy·상호인증·해시 협상·8상태 비동기 머신, 두 가지 호출 진입 경로 — atr 자동 트리거 vs 사용자 명시적, 와이어 메시지 6종 시퀀스 다이어그램, OpenSSL EVP_MAC 의존성과 SPDK_CONFIG_HAVE_EVP_MAC 게이트) + g_digests/g_dhgroups 테이블(SHA-256/384/512 + ffdhe2048~8192 RFC 7919) + 공개 사전 검색 7종(get_digest_id/name + get_dhgroup_id/name + get_digest_length, 양방향 변환) + ★ 상태머신 헬퍼: nvme_auth_set_state(state_names 디버그 배열 unused attribute), nvme_auth_set_failure(첫 status 보존 + AWAIT_FAILURE2 vs DONE 분기), nvme_auth_print_cpl(sct/sc 진단), nvme_auth_get_seqnum(★ RAND 시드 + 0 wrap → 1 강제, ctrlr 단위 lock) + ★ 키 변환: nvme_auth_transform_key(NONE 모드 raw 복사 + SHA-256/384/512 HMAC(key, nqn || "NVMe-over-Fabrics") 도메인 분리), nvme_auth_get_key(★ DHHC-1:HH:base64: 형식 파서 + 36/52/68B 사이즈 검증 + ★ CRC32 IEEE 검증 + spdk_memset_s 보안 클리어) + ★★ nvme_auth_augment_challenge(NULL key→cval 단순 복사, 있으면 caval = HMAC(MD(key), cval) — DH secret으로 nonce 보강) + ★★ spdk_nvme_dhchap_calculate(공개 API, 8 입력 누적 HMAC: caval || seq || tid || scc || type || nqn1 || NUL || nqn2, type "HostHost"/"Controller" 도메인 분리) + DH 키 관리 5종(generate_dhkey "DHX" + ffdhe* 그룹, dhkey_free NULL-safe, get_pubkey BN_bn2binpad 패딩, nvme_auth_get_peerkey OSSL_PARAM_BLD + EVP_PKEY_dup ctx 종속성 끊기, derive_secret set_dh_pad(1) 길이 일관성) + 메시지 I/O: ★★ nvme_auth_submit_request(★ reserved_req 사용 — 일반 풀 고갈 시에도 인증 가능, AUTH Send vs Recv SQE 분기, opcode=FABRIC + fctype + spsp0/1=1 + secp=NVME + tl/al), recv_message(dma_data 0 클리어 + RECV 트리거), send_failure2(COMMON + FAILURE2 + tid + rc/rce), nvme_auth_check_message(DHCHAP id 일치 vs failure1 자동 처리 vs INCORRECT_PROTOCOL_MESSAGE) + 프로토콜 메시지 5종: nvme_auth_send_negotiate(g_digests/g_dhgroups 정책 필터 + descriptors[0] hash_id_list/dhg_id_list 채움 + sc_c=DISABLED + napd=1), nvme_auth_check_challenge(7단계 검증 — type/id + tid + seqnum≠0 + hash_id 지원/일치 + dhgroup별 dhvlen 검증 + 정책 허용), ★★★ nvme_auth_send_reply(7단계 코어 — auth->hash 보존, DH 키페어 생성 + pubkey 추출 + secret 도출, dup된 PSK + ckey, "HostHost" rval 계산, 상호인증 시 seqnum 채번 + RAND ctrlr_challenge + "Controller" 미리 계산해 auth->challenge에 보존, in-place dma_data 재사용으로 reply 빌드 — rval[0..hl] 호스트 응답 + rval[hl..2hl] ctrlr_challenge cvalid=0여도 슬롯 강제 + rval[2hl..2hl+publen] host pubkey, cvalid + dhvlen + seqnum 헤더), nvme_auth_check_success1(ctrlr_key 있으면 rvalid=1 강제 + hl 일치 + ★ memcmp(msg->rval, auth->challenge) — 컨트롤러 신원 증명의 정점), nvme_auth_send_success2(상호인증 완료 통지) + ★★★★ nvme_fabric_qpair_authenticate_poll(8 상태 머신 driver — NEGOTIATE→AWAIT_NEGOTIATE→AWAIT_CHALLENGE→AWAIT_REPLY→AWAIT_SUCCESS1→AWAIT_SUCCESS2/AWAIT_FAILURE2→DONE, in_auth_poll 재진입 가드, do-while + prev_state 비교로 상태 전이 시 즉시 다음 단계 진행 패턴, NEGOTIATE 의도적 early return -EAGAIN 이유 명시, 각 AWAIT_*에서 nvme_wait_for_completion_poll의 -EAGAIN/error/0 3분기, AWAIT_SUCCESS1에서 ctrlr_key 유무로 success2 vs DONE 분기, AWAIT_SUCCESS2/FAILURE2 같은 처리로 DONE 도달, DONE에서 fabric_poll_cleanup + auth_cleanup + cb_fn 호출) + nvme_fabric_qpair_authenticate_async(dhchap_key/ascr 검증, status calloc + dma_data spdk_zmalloc, tid ctrlr 단위 채번, 상태 NEGOTIATE + kick-start polling, -EAGAIN→0 정규화) + spdk_nvme_qpair_authenticate(사용자 노출, EALREADY 동시진행 방지, 트랜스포트 위임 후 cb_fn 등록) + SPDK_LOG_REGISTER_COMPONENT(nvme_auth) 전부 상세 주석. **결과**: NVMe-oF 호스트의 DH-HMAC-CHAP 인증 전 메커니즘이 주석만으로 완전 추적 가능 — PSK keyring → DHHC-1 파싱 → 8상태 머신 → DH 키 교환 → HMAC 응답 계산 → 상호 인증 → cleanup의 모든 단계 + 실패 분기 + 재진입 방어 + 보안 메모리 클리어 패턴. nvme_fabric.c의 atr/ascr 후속 흐름 완성. |
| ☑ | lib/nvme/nvme_discovery.c | 2026-04-28 **완료** (병렬 agent) — 원본 167줄 → 주석 후 472줄. 4개 함수(get_log_page_completion_final/completion/discovery_log_header_completion/spdk_nvme_ctrlr_get_discovery_log_page) + nvme_discovery_ctx 6필드 모두 멀티라인 + 4섹션 블록. ★ NVMe-oF Discovery Log Page (LID 0x70) 클라이언트 — 호스트 attach Phase 1 토폴로지 발견. **3단계 콜백 체인**(header→full page→genctr 재조회) + atomic snapshot 보장 (start/end_genctr 비교 → 변동 시 재시작). |
| ☑ | lib/nvme/nvme_cuse.c | 2026-04-28 **완료** (병렬 agent) — 원본 1556줄 → 주석 후 2559줄. ★ 49개 static + 5개 public API = 54개 함수 보강 + 4섹션 블록. CUSE/libfuse3 기반 char device 시뮬레이션, NVMe ioctl(ADMIN_CMD/IO_CMD/SUBMIT_IO/RESET/RESCAN/BLK*GET/GET_TRANSPORT)을 SPDK admin/IO 명령으로 변환. CUSE thread (단일, do-while + spdk_fd_group_wait 500ms + eventfd 통지) + nvme_io_msg.c 채널을 통한 reactor 위임. 사용자 도구(nvme-cli/nvme list/smartctl)가 SPDK 관리 NVMe device에 접근 가능. SPDK_CONFIG_HAVE_FUSE3 매크로 가드. cuse_device 11필드/cuse_io_ctx 9필드 모두 멀티라인. |
| ☑ | lib/nvme/nvme_ctrlr_ocssd_cmd.c | 2026-04-28 **완료** (병렬 agent) — 원본 69줄 → 주석 후 219줄. 2개 함수(spdk_nvme_ctrlr_is_ocssd_supported, spdk_nvme_ocssd_ctrlr_cmd_geometry) + 4섹션 블록. ★ Open-Channel SSD 1.2/2.0 vendor-specific admin (GEOMETRY opcode 0xE2). NVME_QUIRK_OCSSD + CNEX Labs vid + ns vendor_specific[0]==0x1 휴리스틱. nvme_ctrlr_cmd.c와 동일한 lock→풀 alloc→SQE 빌드→submit 패턴. PRP/IOVA bounce buffer, 4096B payload 검증. |
| ☑ | lib/nvme/nvme_opal.c | 2026-04-28 **완료** (병렬 agent) — 원본 2568줄 → 주석 후 4315줄 (+1747줄). ★ 50개 함수 보강 + 4섹션 블록. **TCG Opal SSC 2.0 SED 클라이언트** — NVMe security_send/receive (opcode 0x81/0x82) 위에 TCG SWG TLV 토큰 스트림으로 암호화/잠금/PIN 관리. **Session 핸드셰이크 4단계**: (1) Discovery — SECURITY_RECEIVE(SECP_INFO/TCG, BaseComID 획득), (2) StartSession(HSN 발급, SP_UID, PIN, Authority_UID → TSN 발급), (3) 메서드 호출(CALL <obj_UID> <method_UID>), (4) EndSession(EOS 토큰). **함수 카테고리**: NVMe security 콜백 체인 5종, TCG SWG Token 빌더 7종(u8/u64/bytestring/short atom/medium atom/finalize), 응답 파서 12종(tiny/short/medium/long token + status), Session/Auth 4종, Locking/PIN/Key 16종, 유틸 + Discovery 헬퍼, **공개 API 16종**(take_ownership/revert_tper/activate_locking_sp/lock_unlock/setup_locking_range/enable_user/add_user/set_passwd/erase 등). **TakeOwnership 핸드셰이크**: ANYBODY 세션 → MSID GET → SID 세션 → C_PIN_SID Set → cleanup + PIN 메모리 즉시 0 클리어. opal_send_recv 동기 spin-poll. |
| ☑ | lib/nvme/nvme_quirks.c | 2026-04-28 **완료** (병렬 agent) — 원본 163줄 → 주석 후 392줄. 2개 함수(pci_id_match, nvme_get_quirks) + struct nvme_quirk 모든 필드 멀티라인 + 정적 배열 19행 모두 quirk 의미 주석 + 4섹션 블록. ★ 디바이스별 quirk 테이블 — 컨트롤러 init 단계 PCI ID 5튜플 룩업, NVME_QUIRK_* 19종 비트마스크 매핑. sentinel-종료 + 와일드카드 + PRINT_QUIRK 디버그 로그 매크로. |
| ☑ | lib/nvme/nvme_zns.c | 2026-04-28 **완료** (병렬 agent) — 원본 255줄 → 주석 후 678줄. 17개 함수(getters 8 + zone_append 4 + mgmt recv/send + 5 zone actions + set_zone_desc_ext) + 4섹션 블록. ★ NVMe TP 4053 ZNS public API — 호스트 → nvme_ns_cmd.c → qpair_submit 경로. opcode 0x7D/0x7A/0x79, ZASL/lbafe/mor/mar 0-based 인코딩, zone state machine. |
| ☑ | lib/nvme/nvme_util.c | 2026-04-28 **완료** (병렬 agent) — 원본 191줄 → 주석 후 526줄. 3개 함수(transport_id_usage, trid_entry_parse, build_name) + 4섹션 블록. ★ NVMe CLI 옵션 공용 유틸 — usage 출력, trid 문자열 파싱(ns/hostnqn/alt_traddr 확장 키), controller+ns 라벨 빌드. -r 옵션 파싱 → spdk_nvme_probe → attach_cb의 build_name 경로. |
| ☑ | lib/nvme/nvme_stubs.c | 2026-04-28 **완료** (병렬 agent) — 원본 77줄 → 주석 후 261줄. 9개 stub 함수(CUSE 5 + RDMA 1 + EVP_MAC auth 3) + 4섹션 블록. SPDK_CONFIG 빌드 옵션 비활성 시 링크 호환 stub. -ENOTSUP vs abort() 정책 분기 (RDMA fail-fast). |
| ☑ | lib/nvme/nvme_ns_ocssd_cmd.c | 2026-04-28 **완료** (병렬 agent) — 원본 205줄 → 주석 후 484줄. 6개 함수(_nvme_ocssd_ns_cmd_vector_rw_with_md + vector_reset/write/write_with_md/read/read_with_md/copy) + 4섹션 블록. ★ Open-Channel SSD vector I/O 빌더 (opcode 0x90/0x91/0x92/0x93). nvme_ns_cmd.c의 OCSSD 변형. cdw10/11/12/14/15 인코딩 + num_lbas==1 vs 다중 인라인 LBA 최적화 + 0-based 인코딩 (cdw12 = num_lbas-1). |
| ☑ | lib/nvme/nvme_vfio_user.c | 2026-04-28 **완료** (병렬 agent) — 원본 361줄 → 주석 후 874줄. 14개 함수 + 1 구조체 + vtable + 4섹션 블록. ★ vfio-user 트랜스포트 — BAR/Config을 socket RPC로 외부화, PCIe 트랜스포트의 control-plane 변형. spdk_pci_addr 대신 vfio-user socket path 사용. nvme_vfio_ctrlr 상속, BAR0 doorbell mmap, CMD register 0x404(BME+INTx-disable), ASQ/ACQ/AQA 셋업. **핫패스는 nvme_pcie_* 재사용** — vtable이 vfio-고유 cold-path만 덮어씀. SPDK_CONFIG_VFIO_USER 매크로 가드. |

| 상태 | 경로 | 비고 |
|------|------|------|
| ☐ | lib/bdev/bdev_internal.h | bdev 내부 |
| ☐ | lib/bdev/bdev.c | bdev 코어 |
| ☐ | lib/bdev/bdev_rpc.c | bdev RPC |
| ☑ | lib/bdev/bdev_zone.c | 2026-04-28 **확인** (병렬 agent) — 이미 완료 상태 (원본 208 → 주석 후 607줄). 13개 함수 (getter 7 + 비동기 명령 6: get_zone_info/zone_management/zone_append 등). bdev_io 캡슐화 → submit_request → NVMe Zone 명령(0x79/0x7A/0x7D). |
| ☑ | lib/bdev/part.c | 2026-04-28 **완료** (병렬 agent) — 원본 691 → 1481줄. 22개 함수 + part_base/part/part_channel 구조체 (16필드 멀티라인). partition vbdev 공통 코어 — offset+length로 base bdev 분할. GPT/split/lvol 등이 라이브러리로 활용. DIF Reference Tag remap + thread affinity (base->thread, base_ch). |
| ☑ | lib/bdev/scsi_nvme.c | 2026-04-28 **완료** (병렬 agent) — 원본 234 → 483줄. 1개 함수(spdk_scsi_nvme_translate, 거대한 SCT/SC switch 표 라인 단위 주석). NVMe completion(SCT,SC) → SCSI sense(sc,sk,asc,ascq) 변환. iSCSI/vhost-scsi target에서 NVMe backend 사용 시 어댑터. SCT 4종(GENERIC/COMMAND_SPECIFIC/MEDIA_ERROR/VENDOR_SPECIFIC) 1차 분기 + SC 2차 분기. |
| ☑ | lib/bdev/vtune.c | 2026-04-28 **확인** (병렬 agent) — 이미 완료 상태 (원본 21 → 주석 후 88줄). Intel VTune ITT 정적 구현 thin wrapper. SPDK_CONFIG_VTUNE 빌드 시 lib/bdev에 ITT 한 번 포함. 함수/구조체 없이 #include + 경고 억제 #pragma만. |

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

1. **NVMe 드라이버 다섯 기둥 완결 이후 확장 작업**:
   - ☑ `lib/nvme/nvme_fabric.c` (2026-04-26 완료) — Fabrics CONNECT/AUTH/Property R/W/Discovery 공통 로직
   - ☑ `lib/nvme/nvme_poll_group.c` (2026-04-28 완료) — transport poll group들을 묶는 상위 그룹
   - ☑ `lib/nvme/nvme_auth.c` (2026-04-28 완료) — DH-HMAC-CHAP 인증 8상태 머신 + DH 키교환 + HMAC 응답
   - ☑ `lib/nvme/nvme.c` (2026-04-28 완료) — probe/attach/detach 진입점 + driver_init + admin polling 헬퍼
   - ◐ `lib/nvme/nvme_ctrlr.c` (2026-04-28 v2) — 핵심 8종 + qpair 관리 10종 = 18종 보강. 잔여: opts 큰 함수/features/reset/AER/multi-process/process_init sub-callback/public APIs ← **다음 세션 Part 3 계속**
   - ☑ `lib/nvme/nvme_ctrlr_cmd.c` (2026-04-28 완료, 병렬 agent) — 28개 admin 커맨드 빌더 + abort 7종
   - ☑ `lib/nvme/nvme_ns.c` (2026-04-28 확인, 병렬 agent) — 이미 완료 상태 검증
   - ☑ `lib/nvme/nvme_io_msg.c` (2026-04-28 완료, 병렬 agent) — 외부 스레드 메시지 채널
   - ☑ `lib/nvme/nvme_util.c` (2026-04-28 완료, 병렬 agent) — CLI 옵션 공용 유틸
   - ☑ `lib/nvme/nvme_quirks.c` (2026-04-28 완료, 병렬 agent) — PCI ID 기반 quirk 테이블
   - ☑ `lib/nvme/nvme_zns.c` (2026-04-28 완료, 병렬 agent) — NVMe TP 4053 ZNS public API
   - ☑ `lib/nvme/nvme_discovery.c` (2026-04-28 완료, 병렬 agent) — Discovery Log Page 클라이언트
   - ☑ `lib/nvme/nvme_pcie_internal.h` (2026-04-28 검증, 병렬 agent) — 712줄, 이미 완료 상태
   - ☑ `lib/nvme/nvme_stubs.c` (2026-04-28 완료, 병렬 agent) — 9개 stub (CUSE/RDMA/EVP_MAC)
   - ☑ `lib/nvme/nvme_ctrlr_ocssd_cmd.c` (2026-04-28 완료, 병렬 agent) — OCSSD admin GEOMETRY
   - ☑ `lib/nvme/nvme_ns_ocssd_cmd.c` (2026-04-28 완료, 병렬 agent) — OCSSD vector I/O
   - ☑ `lib/nvme/nvme_vfio_user.c` (2026-04-28 완료, 병렬 agent) — vfio-user 트랜스포트
   - ☑ `lib/nvme/nvme_cuse.c` (2026-04-28 완료, 병렬 agent) — 54개 함수, libfuse3 char device
   - ☑ `lib/nvme/nvme_opal.c` (2026-04-28 완료, 병렬 agent) — 50개 함수, TCG Opal SED 클라이언트
   - `lib/nvme/nvme_ns.c` — namespace identify 및 설정
   - `lib/nvme/nvme_ctrlr_cmd.c` — admin 커맨드 빌더 (Identify, Set Features, Get Log Page 등)

2. **추가 I/O 경로 트랜스포트** (필요 시):
   - `lib/nvme/nvme_rdma.c` — RDMA 트랜스포트 (NVMe-oF)
   - `lib/nvme/nvme_tcp.c` — TCP 트랜스포트 (NVMe-oF)
   - `lib/nvme/nvme_vfio_user.c` — vfio-user 트랜스포트

2. **부분완료(◐) 파일 마무리**:
   - `include/spdk/bdev.h` (2551) — 아직 ◐. seek_offset, histogram_enable_ext 등 세부 API 잔여
   - `include/spdk/bdev_module.h` — claim API 프로토타입, param 구조체 잔여
   - `lib/nvme/nvme_internal.h` — fabric/rdma/tcp 특화 프로토타입, namespace 세부 API

3. **Phase 0 남은 파일**:
   - `include/spdk/dif.h`         — T10 DIF
   - `include/spdk/histogram_data.h` (286)
   - `include/spdk/log.h` (443)
   - `include/spdk/json.h` / `jsonrpc.h` / `rpc.h` — RPC 스택
   - `include/spdk/trace.h` / `trace_parser.h`
   - `include/spdk/tree.h` (842) — BSD tree 매크로 (전용 세션)

4. **Phase 1 공개 헤더**: env.h, thread.h, event.h, init.h, scheduler.h, conf.h, dma.h, env_dpdk.h

5. **세션 종료 전 반드시**: 본 `.md`에서 아래 항목 갱신
   - "마지막 세션 요약"
   - "현재 진행 중 파일" (중간에 멈췄다면 위치와 함께 기록)
   - "다음에 할 일"

## 현재 진행 중 파일

- ◐ **lib/nvme/nvme_ctrlr.c** — 5997 → 6510 → 6778 → 7289 → 7840줄. v1 8종 + v2 qpair 10종 + v3 features/fail 9종 + v4 shutdown 9종 + v5 disconnect/reconnect 12종 = **48종 보강**. 잔여 ~58 함수. 다음 세션 Part 6에서 reinitialize/reconnect_poll/disable/reset/AER 우선 진행.

## 최근 완료 파일 (역순 최대 30개)

- 2026-04-29 · **6차 병렬 agent 4개 작업 동시 실행** — 22개 파일 + nvme_ctrlr Part 5 일괄 완료. **누적**: 원본 4,302줄 → 주석 후 7,776줄 (+3,474줄, 1.81×). 4섹션 블록 22/22 PASS.

  **agent A — lib/util/ 작은 파일 12개 (894 → 2,902, +2,008)**:
    strerror_tls.c (15→81), crc32_ieee.c (21→107), math.c (46→133), md5.c (62→185), crc32.c (79→213), hexlify.c (85→217), zipf.c (111→277), crc32c.c (133→308), fd.c (134→282), file.c (145→322), xor.c (145→316), crc64.c (169→261). **인사이트**: CRC 4종(IEEE blob/CRC32C NVMe-oF HDGST·DDGST·iSCSI Digest·DIF guard/CRC64 NVMe 2.0 64-bit PI guard) 다항식 spec + ISA-L/SSE4.2/ARM CRC32/SW 4가지 빌드 분기, MD5 = iSCSI CHAP, hexlify = NQN/UUID·DH-HMAC-CHAP, zipf = bdevperf 워크로드 모사 (Gray '94), xor = RAID-5 ISA-L 32B 정렬, fd/file = aio bdev + env_dpdk PCI sysfs.

  **agent B — lib/util/ 중간 파일 6개 (1,609 → 2,981, +1,372)**:
    uuid.c (209→455), iov.c (243→489), base64.c (250→441), net.c (209→376), cpuset.c (329→585), pipe.c (369→635). **인사이트**: pipe.c 는 형식적으로는 SPSC ring 이지만 SPDK 일반 사용은 producer(socket recv)+consumer(PDU parser) 가 같은 reactor 스레드에서 직렬 호출되어 atomic load 없이 정확성 유지 — lock-free 근거 명시. cpuset.c 의 parse_mask(/sys/cpumask 콤마 표기) + parse_list([a,b-c] 표기) 가 동일 비트맵으로 정규화 → --cpumask 옵션 두 표현 모두 지원.

  **agent C — lib/log/ 전체 (610 → 1,694, +1,084)**:
    log.c (316→855), log_flags.c (150→410), log_deprecated.c (144→429). **★ log.c sink + level 필터 메커니즘**: spdk_vlog 가 g_log_opts.log 콜백 등록 시 즉시 위임 (포맷팅·필터링 SPDK가 손 떼고), 미등록 시 두 단계 컷오프 (g_spdk_log_print_level → stderr / g_spdk_log_level → syslog). 메시지 빌드는 1KB 스택 버퍼 vsnprintf 시도 → 초과 시 va_copy + vasprintf 동적 할당으로 잘림 회피. log_flags.c = SPDK_LOG_REGISTER_COMPONENT constructor 자동 등록 비트 토글, log_deprecated.c = N회 fire 마다 출력 + RPC log_get_deprecation_history.

  **agent D — lib/nvme/nvme_ctrlr.c Part 5: disconnect/reconnect chain 12종 (7289→7840, +551)**:
    set_state, set_state_quiet (silent 변형, WAIT_FOR_* 폴링 재진입용), free_zns_specific_data, free_iocs_specific_data, free_doorbell_buffer (DBBUF NVMe §5.7 shadow/eventidx 페이지), set_doorbell_buffer_config_done, set_doorbell_buffer_config (DBBUF 두 hugepage 할당 + PRP 추출 + 발행), abort_queued_aborts (queued_aborts STAILQ 를 SC_ABORTED_SQ_DELETION 통보·폐기), ★ disconnect (reset chain 시작점), disconnect_done (캐시 invalidate + state=DISCONNECTED), spdk_nvme_ctrlr_disconnect (외부 lock wrapper), ★ spdk_nvme_ctrlr_reconnect_async (lock 보유 채 return, prepare_for_reset=false, state=INIT → process_init 재실행). **★ 비대칭 lock 페어링**: reconnect_async 가 lock acquire / reconnect_poll_async 가 release 명시. NVMe Spec §5.7(DBBUF)/§5.1·5.2(Abort/AER)/§7.3(Reset Processing) 인용.

  **누적 — nvme_ctrlr.c 48종 보강**: v1 핵심 8종 + v2 qpair 10종 + v3 features/fail 9종 + v4 shutdown 9종 + v5 disconnect/reconnect 12종. **잔여 ~58 함수** (Part 6): reinitialize_io_qpair, reconnect_poll_async, disable/disable_poll, fail_io_qpairs, ★ spdk_nvme_ctrlr_reset/reset_subsystem (사용자 reset API), set_trid/set_remove_cb, AER 처리 일습.

- 2026-04-29 · **5차 병렬 agent 4개 작업 동시 실행** — 7개 파일 일괄 완료:
  - **include/spdk/event.h** (384→851, +467) — agent A. 14함수 + spdk_app_opts 40+ 필드 + spdk_app_parse_args_rvals enum 3값 + 매크로 SPDK_APP_GETOPT_STRING/SPDK_STATIC_ASSERT + typedef 3 + 전방선언 2. **인사이트**: spdk_app_opts 의 4중 ABI 안전장치 (`__attribute__((packed))` + reserved 홀 + caller opts_size + STATIC_ASSERT) 명시.
  - **include/spdk/init.h** (154→366, +212) — agent A. 8함수 (rpc_initialize/finish/server_finish/pause/resume + subsystem_init/load_config/fini/exists) + typedef 2 + spdk_rpc_opts 3필드.
  - **include/spdk/scheduler.h** (319→605, +286) — agent A. 9함수 + 4구조체(governor 11멤버, scheduler 7멤버, thread_info 4필드, core_info 9필드, governor_capabilities) + 매크로 3 (REGISTER × 2, MAX_LCORE_FREQS). **인사이트**: 모든 vtable 콜백이 단일 "scheduling reactor" 위에서만 실행되어 내부 동기화 불필요.
  - **include/spdk/conf.h** (187→448, +261) — agent B. 14함수 + opaque 구조체 4개. **인사이트**: SPDK 18.04 이후 JSON-RPC 로 전환 → INI 파서가 사실상 deprecated 동결 API.
  - **include/spdk/env_dpdk.h** (103→269, +166) — agent B. 5함수 + 1구조체(6필드). **인사이트**: 외부 앱이 이미 rte_eal_init 호출한 임베드 시나리오용 우회 진입점, post_fini 가 의도적으로 EAL 종료 안 함 (소유권 분리), legacy_mem 플래그가 vfio DMA 매핑 방식 결정.
  - **lib/thread/thread_internal.h** (104) — agent B 검증. 이미 4섹션 + 모든 필드 멀티라인 + include 전부 주석 완비 상태로 보강 불필요.
  - **lib/thread/iobuf.c** (916→1510, +594) — agent C. 22함수 + 6구조체 (iobuf_channel_node/channel/module/node/iobuf/get_stats_ctx) 모든 필드 멀티라인 + 매크로 IOBUF_FOREACH_NUMA_ID/SET_FIELD + STAILQ pop/push + spdk_ring + DPDK numa_id 분기 인라인. **★ 핵심 인사이트**: get() hot path = per-thread STAILQ 캐시 hit (락·atomic 없음) → miss 시 IOBUF_BATCH_SIZE 단위로 글로벌 lockless ring 에서 batch dequeue (DPDK Magazine 패턴). 글로벌 풀도 비면 reactor 공유 wait queue 등록 후 NULL. put() 은 wait queue 비면 cache push (cache_size+batch 초과 시만 batch flush, ring 슬롯·캐시라인 효율 목적), 비어있지 않으면 글로벌 ring 우회하고 동일 thread waiter 에 buf 직접 전달. lockless 근거는 모든 함수 진입의 `spdk_io_channel_get_thread(ch->parent) == spdk_get_thread()` assert 로 명시.

- 2026-04-29 · **lib/nvme/nvme_ctrlr.c [◐ 부분 v4 — Part 4: shutdown chain + enable/state_string 9종]** — 6778 → 7289줄 (+511). agent D 병렬. shutdown 비동기 5-step chain 완결:
  - **★ shutdown 5-step chain**: nvme_ctrlr_shutdown_async → set_cc_done → get_cc_done → poll_async → get_csts_done. 각 콜백 헤더에 직전/직후 함수 도식 명시.
  - **nvme_ctrlr_shutdown_set_cc_done**: CC.SHN(Shutdown Notification) 비트 set 완료 콜백. 다음=GET_CSTS 단계.
  - **nvme_ctrlr_shutdown_get_cc_done**: 현 CC 값 read 완료, SHN 비트 mask 후 NORMAL_SHUTDOWN(01b) 또는 ABRUPT_SHUTDOWN(10b) write 결정.
  - **nvme_ctrlr_shutdown_async**: shutdown chain 시작점. is_disconnecting 검사 → state CHECK_SHUTDOWN.
  - **nvme_ctrlr_shutdown_get_csts_done**: CSTS.SHST(Shutdown Status) 비트 검사 — 00=Normal, 01=Occurring, 10=Complete. complete 시 chain 종료, 그 외 poll 재진입.
  - **nvme_ctrlr_shutdown_poll_async**: shutdown 진행 폴링. RTD3E (NVMe §5.15.2.2 Runtime D3 Entry latency) 기반 timeout 계산.
  - **nvme_ctrlr_get_ready_timeout**: CAP.TO(Timeout) × 500ms 환산.
  - **nvme_ctrlr_set_cc_en_done**: CC.EN=1 write 완료. 다음=CHECK_EN_DONE.
  - **★ nvme_ctrlr_enable**: controller bring-up 의 enable 단계. CAP.MQES/CSS 검증 + CC.IOSQES/IOCQES/AMS/SHN/CSS 비트필드 빌드 + AB(Arbitration Burst)/MPS(Memory Page Size)/EN 통합 → 비동기 write.
  - **nvme_ctrlr_state_string**: enum nvme_ctrlr_state → 사람용 문자열 매핑 테이블 (~80개 case).

  **NVMe Spec 인용**: §7.6.2(Shutdown Processing), §3.1.5(CC), §3.1.6(CSTS), §3.1.1(CAP), §5.15.2.2(RTD3E), §7.3(Reset). CC.SHN/EN, CSTS.SHST 비트 의미를 spec 값과 함께 명시.

  **누적 — nvme_ctrlr.c 36종 보강**: v1 핵심 8종 + v2 qpair 10종 + v3 features/fail 9종 + v4 shutdown/enable/state_string 9종. **잔여 ~70 함수** (다음 세션 Part 5): set_state/set_state_quiet, free_zns/iocs/doorbell_buffer, set_doorbell_buffer_config_done/cfg, abort_queued_aborts, ★ disconnect/disconnect_done, ★ reconnect_async/reinitialize_io_qpair/reconnect_poll_async, disable/disable_poll, fail_io_qpairs, **★ spdk_nvme_ctrlr_reset/reset_subsystem (사용자 reset API)**, set_trid/set_remove_cb, AER 처리 일습.

- 2026-04-29 · **4차 병렬 agent 7개 파일 일괄 완료** — 6510줄 → 6778줄 (+268줄). features 비트맵 + fail 진입점 보강:
  - **nvme_ctrlr_set_supported_log_pages**: log_page_supported[] 비트맵 결정. mandatory 3종(Error/Health/Firmware Slot) + LPA.cses(Command Effects) + CMIC.anars(ANA, 옵션이면 즉시 read+parse) + ctratt.fdps(FDP 4종) + Intel/PCIe 분기로 SET_SUPPORTED_INTEL_LOG_PAGES 또는 직접 SET_SUPPORTED_FEATURES 로 전이.
  - **nvme_ctrlr_set_intel_supported_features**: Intel vendor FID 7종 (MAX_LBA C1h / NATIVE_MAX_LBA C2h / POWER_GOVERNOR C6h / SMBUS C8h / LED C7h / TIMED_WORKLOAD D5h / LATENCY_TRACKING E2h) 비트맵만 활성화 (실제 admin 명령 발행 안 함).
  - **★ nvme_ctrlr_set_arbitration_feature**: 실제 Set Features (FID=01h) admin 발행. AB(3-bit) + WRR HPW/MPW/LPW 가중치 cdw11 비트필드 빌드 + completion poll 동기 대기. AB=0 / >7 / WRR 미지원 분기 처리.
  - **nvme_ctrlr_set_supported_features**: feature_supported[] 비트맵. mandatory 9종 무조건 + 옵션 3종(VWC/APST/HMB) Identify Ctrlr 비트로 분기 + Intel 추가 + 끝에 set_arbitration_feature 동기 호출.
  - **nvme_ctrlr_set_host_feature_done**: Set Features (FID=16h Host Behavior) 비동기 완료 콜백. tmp_ptr free → CQE error 검사 (실패=ERROR state) → 성공 시 비트맵 마킹 + SET_DB_BUF_CFG 전이.
  - **nvme_ctrlr_set_host_feature**: ctratt.elbas 분기 (미지원=skip), DMA buffer 4KB 정렬 zmalloc + state WAIT_FOR_SET_HOST_FEATURE → host->lbafee=1 + 비동기 admin 발행. error label 4단계 cleanup (free + state ERROR).
  - **spdk_nvme_ctrlr_is_failed**: is_failed 플래그 단순 read public API (락 없음).
  - **★ nvme_ctrlr_fail**: 내부용 fail 진입점 (호출자 lock 보유 가정). hot_remove → is_removed / 중복 호출 idempotent / is_disconnecting 시 skip / state=ERROR + admin disconnect. ★ "플래그만 set, in-flight IO 실패 보고는 process_completions 가 lazy 처리" 패턴 명시.
  - **spdk_nvme_ctrlr_fail**: 외부 사용자용 lock wrapper (nvme_ctrlr_lock → fail → unlock).

  **결과 — features 단계 + fail 진입점 완결**: bring-up 의 SET_SUPPORTED_LOG_PAGES → SET_SUPPORTED_INTEL_LOG_PAGES → SET_SUPPORTED_FEATURES → SET_HOST_FEATURE → SET_DB_BUF_CFG 전이가 주석으로 추적 가능. 또 fail 의 두 layer (내부 lock-held 진입 vs 외부 lock-wrapped 진입) 가 명확히 분리.

  **잔여 함수** (다음 세션 Part 4): shutdown_set_cc_done/get_cc_done/async/get_csts_done/poll_async (5종 비동기 chain), get_ready_timeout, set_cc_en_done/enable, state_string(string table), set_state_quiet, free_zns/iocs/doorbell_buffer, set_doorbell_buffer_config_done/cfg, abort_queued_aborts, disconnect/disconnect_done, reconnect_async/reinitialize/reconnect_poll_async, disable/disable_poll, fail_io_qpairs, ★ spdk_nvme_ctrlr_reset/reset_subsystem (사용자 reset API), set_trid, set_remove_cb, AER 처리 일습.

- 2026-04-29 · **4차 병렬 agent 7개 파일 일괄 완료** — nvme_tcp.c(3416→4813, 50종 신규), include 헤더 6개: json.h(353→843), jsonrpc.h(357→715), rpc.h(156→363), trace.h(506→1153), trace_parser.h(131→314), dma.h(472→901). 3개 병렬 agent 동시 실행. **누적**: 원본 5391줄 → 주석 후 9102줄 (+3711줄), 약 130 함수/매크로/구조체 보강. 4섹션 블록 7/7 검증. **핵심 추가**: NVMe-oF TCP의 PDU 인코딩 + R2T flow control + TLS PSK HKDF + recv 상태머신, JSON-RPC 2.0 server/client + SPDK_RPC_REGISTER constructor 자동 등록 + STARTUP/RUNTIME phase gating, lockless trace circular buffer per-lcore + /dev/shm dump + spdk_trace CLI 후처리, DMA memory domain 추상화 + zero-copy I/O accel_sequence 통합.

- 2026-04-28 · **3차 병렬 agent 7개 파일 일괄 완료** — include/spdk/log.h(443→886, 검증), include/spdk/histogram_data.h(286→609), include/spdk/dif.h(492→999), lib/bdev/bdev_zone.c(208→607, 검증), lib/bdev/part.c(691→1481), lib/bdev/scsi_nvme.c(234→483), lib/bdev/vtune.c(21→88, 검증). 2개 병렬 agent 동시 실행. **누적 통계**: 원본 2375줄 → 주석 후 5153줄 (+2778줄), 70+ 함수 보강 + 3개 파일(log.h/bdev_zone.c/vtune.c) 이미 완료 상태 검증. 4섹션 블록 모두 검증 완료. 핵심 추가: T10 DIF/DIX 보호정보 처리(Guard/AppTag/RefTag, PI Type 1/2/3, DIF/DIX 두 모드), histogram의 logarithmic range×linear bucket + per-thread merge, partition vbdev 공통 코어(offset+length 분할 + DIF Reference Tag remap + thread affinity), SCSI sense ↔ NVMe SCT/SC 양방향 변환.

- 2026-04-28 · **2차 병렬 agent 7개 파일 일괄 완료** — nvme_pcie_internal.h (712줄 검증), nvme_stubs.c (77→261, 9종), nvme_ctrlr_ocssd_cmd.c (69→219, 2종), nvme_ns_ocssd_cmd.c (205→484, 6종), nvme_vfio_user.c (361→874, 14종+vtable), nvme_cuse.c (1556→2559, 54종), nvme_opal.c (2568→4315, 50종). 4개 병렬 agent 동시 실행. **누적 통계**: 원본 5548줄 → 주석 후 8712줄 (+3164줄), 135개 함수 보강. 모든 파일 4섹션 블록 검증 완료. 핵심 추가: TCG Opal SSC 2.0 session 4단계 핸드셰이크 (Discovery → StartSession → 메서드 호출 → EndSession), CUSE/libfuse3 char device 시뮬레이션 + nvme_io_msg 채널 위임, vfio-user 트랜스포트의 socket-RPC BAR + nvme_pcie_* hot-path 재사용, OCSSD vector I/O cdw 0-based 인코딩, SPDK_CONFIG 비활성 시 stub fallback 정책.

- 2026-04-28 · **6개 파일 병렬 agent로 일괄 완료** — nvme_ctrlr_cmd.c (1048→1682, 28종), nvme_io_msg.c (217→752, 7종), nvme_util.c (191→526, 3종), nvme_quirks.c (163→392, 2종+테이블), nvme_zns.c (255→678, 17종), nvme_discovery.c (167→472, 4종). + nvme_ns.c (1582줄, 이미 완료 상태 검증). 4개 병렬 agent 동시 실행으로 효율적 완료. 모든 파일 4섹션 블록 검증 완료 (grep으로 4/4 sections 확인). 핵심 추가: admin 커맨드 빌더의 NVMe spec opcode/CDW 매핑, 외부 스레드 메시지 채널 MP-SC ring 패턴, Discovery Log Page 3단계 콜백 + atomic snapshot, ZNS TP 4053 zone state machine, PCI quirk PRINT_QUIRK 매크로, CLI 옵션 trid 파싱.

- 2026-04-28 · **lib/nvme/nvme_ctrlr.c [◐ 부분 v2 — Part 2: qpair 관리 10종]** — **★ qpair 라이프사이클 사용자 API 완결 ★**. 6265줄 → 6510줄 (+245줄). v1 핵심 8종에 이어 qpair 관리 10종 보강:
  - **spdk_nvme_ctrlr_get_opts**: ctrlr->opts 포인터 노출 (사용자가 수정 가능, 일부 필드는 reset 후 적용)
  - **★ nvme_ctrlr_proc_add_io_qpair**: alloc된 qpair를 현 PID의 active_procs[]에 등록 — multi-process 안전성 (각 프로세스는 자기 qpair만 추적)
  - **★ spdk_nvme_ctrlr_get_default_io_qpair_opts**: 13개 필드 기본값 + ABI 호환 SET_FIELD 매크로. qprio=URGENT, io_queue_size 상속, delay_cmd_submit/async/create_only 기본값 의미.
  - **nvme_ctrlr_io_qpair_opts_copy**: 사용자 opts → 라이브러리 opts 복사. SPDK_STATIC_ASSERT(sizeof==80B)로 새 필드 추가 시 컴파일 가드.
  - **★ nvme_ctrlr_create_io_qpair**: 5단계 — qprio MASK 검증 + AMS=RR이면 URGENT 강제 (스펙 정의) + spdk_nvme_ctrlr_alloc_qid + transport create_io_qpair (PCIe SQ/CQ DMA, RDMA ibv_create_qp 등) + active_io_qpairs 등록 + proc 등록.
  - **★★ spdk_nvme_ctrlr_alloc_io_qpair**: ★ 핵심 사용자 API — 8단계 (state==READY 검증 / 기본값+사용자 overlay / 사용자 sq.vaddr 시 buffer_size 검증 / interrupt+delay_cmd_submit 충돌 / create_io_qpair / create_only 분기 / 자동 connect / 실패 시 4단계 cleanup proc_remove + tailq remove + qid bitmap 반환 + transport delete).
  - **★ spdk_nvme_ctrlr_reconnect_io_qpair**: 끊긴 qpair 재연결. 상태 4분기 — is_removed → -ENODEV (영구), is_resetting/DISCONNECTING → -EAGAIN (재시도), is_failed/DESTROYING → -ENXIO (회복불가), DISCONNECTED 외 → 0 (idempotent), DISCONNECTED → transport connect.
  - **spdk_nvme_ctrlr_get_admin_qp_failure_reason**: admin qpair의 transport_failure_reason 단순 노출 (process_admin_completions가 -EIO 반환 시 사용자 진단용).
  - **nvme_ctrlr_disconnect_qpair**: lock 자동 wrapper — 외부 호출자용 (vs nvme_transport_ctrlr_disconnect_qpair는 lock 직접 보유 호출자용).
  - **★★ spdk_nvme_ctrlr_free_io_qpair**: ★ 핵심 사용자 API — 7단계 (NULL 가드 / **in_completion_context 자기 free 패턴** — cb_fn 안에서 free 호출 시 delete_after_completion_context=1 마킹 후 return → process_completions가 callback 끝나고 실제 free / transport disconnect / async DISCONNECTING 폴링 / DISCONNECTED 검증 / poll_group_remove 같은 프로세스만 / DESTROYING 마킹 + foreign qpair 안전성 검사 — 다른 프로세스 cb_fn 호출 금지 UAF 방지 + 4단계 cleanup).

  **결과 — qpair 라이프사이클 완결**: 사용자가 spdk_nvme_ctrlr_alloc_io_qpair → spdk_nvme_ns_cmd_read 사용 → spdk_nvme_ctrlr_free_io_qpair 까지의 전 경로 + 다중 프로세스 안전성 + in-completion 자기 free 패턴 + reconnect 상태 분기가 모두 주석만으로 추적 가능.

  **잔여 함수** (다음 세션 Part 3): spdk_nvme_ctrlr_get_default_ctrlr_opts (큰 함수), Intel/ANA log pages, supported_features, host_feature, set_arbitration, fail/shutdown_async/poll, reset/reset_subsystem, disconnect/disable, set_num_queues/keep_alive_timeout/host_id, configure_aer/async_event_cb, multi-process(get_process/add/remove/cleanup, proc_get/put_ref), process_init sub-callbacks 9종, public APIs 50+ (get_data/regs_*/alloc_qid/attach_ns/format/firmware/CMB/PMR/boot_partition/security/authenticate).

- 2026-04-28 · **lib/nvme/nvme_ctrlr.c [◐ 부분 v1]** — **★ NVMe Controller 라이프사이클 핵심 8종 보강 ★**. 5997줄 → 6265줄. 5997줄의 거대한 파일이라 다세션 분할 작업 시작. 이번 세션에서 추가/보강한 함수:
  - **★ nvme_ctrlr_state_string**: 상태머신 40+ 상태 → 사람용 문자열 매핑. 상단 doc에서 6개 그룹 분류 다이어그램 (INIT / DISABLE / ENABLE / IDENTIFY / NS DISCOVERY / FEATURES / 최종) + "WAIT_FOR_*" 패턴 의미 (비동기 명령 응답 대기).
  - **_nvme_ctrlr_set_state + nvme_ctrlr_set_state/quiet 트리오**: 상태 전이 + timeout 설정. NVME_TIMEOUT_KEEP_EXISTING/INFINITE 특수값, ms→tick 변환 + overflow 방어 2단계, quiet=true는 같은 상태 반복 진입 시 로그 폭주 방지.
  - **nvme_ctrlr_free_zns/iocs_specific_data + free_doorbell_buffer**: ZNS identify 데이터 + NVMe 1.3+ shadow doorbell 해제 (MMIO 비용 절감 메커니즘).
  - **★★★ nvme_ctrlr_process_init**: ★ controller bring-up 상태머신의 driver. 호출 컨텍스트(reactor 루프 → probe_poll_async → poll_internal), 동작 패턴 3 stage(sleep_timeout 검사 → switch dispatch 동기/비동기 단계 → WAIT_FOR_* timeout 검사), **정상 경로 시퀀스 다이어그램** (INIT_DELAY → CONNECT_ADMINQ → READ_VS → READ_CAP → CHECK_EN → DISABLE/ENABLE → RESET_ADMIN → IDENTIFY → CONFIGURE_AER → SET_KEEP_ALIVE → IDENTIFY_IOCS → GET_ZNS_LOG → SET_NUM_QUEUES → IDENTIFY_ACTIVE_NS → IDENTIFY_NS 반복 → SET_SUPPORTED_LOG/FEATURES → SET_HOST_FEATURE → SET_DB_BUF_CFG → SET_HOST_ID → TRANSPORT_READY → READY) + Reset/Error 분기.
  - **nvme_robust_mutex_init_recursive_shared**: RECURSIVE + ROBUST + PSHARED 3종 속성. vs nvme.c의 init_shared 비교(driver lock은 단순, ctrlr lock은 재귀).
  - **★ nvme_ctrlr_construct**: 7단계 (INIT_DELAY vs INIT 분기로 PCIe quirky vs Fabrics, admin_queue_size 검증/정규화 max/quirk multiple/min, 플래그 0 클리어, 빈 컨테이너 7종 초기화, ctrlr_lock 초기화).
  - **nvme_ctrlr_destruct_finish/destruct_async**: ★ destruct 비동기 시퀀스 — is_destructed 마킹으로 새 attach 거부, queued aborts/AER 취소, IO qpair 강제 정리, doorbell/IOCS data free, shutdown_async 시작 (CC.SHN=01 + CSTS.SHST=10 폴링).

  **남은 작업** (상당량, 다음 세션):
  - opts 헬퍼: spdk_nvme_ctrlr_get_default_ctrlr_opts (큰 함수), get_default_io_qpair_opts, opts_copy
  - qpair 관리: alloc_io_qpair, connect/disconnect_io_qpair, free_io_qpair, reconnect_io_qpair
  - Features 단계: set_intel_log_pages, ANA log, supported_features, host_feature, set_arbitration
  - 실패/리셋: fail, shutdown_async/poll, reset, reset_subsystem, reconnect_async/poll, disable
  - Configure/IDs: set_num_queues, keep_alive_timeout, host_id
  - AER: configure_aer, async_event_cb, process_async_event, complete_queued_async_events
  - Multi-process: get_process, add/remove/cleanup, proc_get/put_ref, get_ref_count
  - process_init sub-callbacks: vs_done, cap_done, check_en, set_en_0, wait_for_ready_0/1, enable_wait_for_ready_1
  - Public APIs (50+): get_data, get_regs_*, alloc_qid/free_qid, attach_ns/detach_ns, format, update_firmware, reserve_cmb/map_pmr, boot_partition_start/write, security_send/recv, get_memory_domains, authenticate

- 2026-04-28 · **lib/nvme/nvme.c [☑ 완료]** — **★ NVMe 드라이버 메인 진입점 + 공용 헬퍼 완전 정복 ★**. 원본 2277줄 → 주석 후 2914줄. 기존 일부 주석 위에 **빠진 함수 30+개 보강**. 핵심 완료:
  - **파일 상단 4섹션 블록**: 드라이버 단일 인스턴스(`g_spdk_nvme_driver`) multi-process hugepage 공유 객체 + probe/attach/detach 진입점 + admin command 동기 polling 헬퍼 + ref counting via robust mutex.
  - **★ Multi-process probe/attach 라이프사이클**: nvme_ctrlr_shared(PCIe만 공유 가능 — BAR mapping 특성), nvme_get_ctrlr_by_trid_unsafe(local + shared 양 리스트 순회), nvme_probe_internal(secondary+PCIe 자동 attach 경로 + lock unlock-during-cb 패턴).
  - **★ ref count 기반 detach**: spdk_nvme_detach 동기 wrapper + spdk_nvme_detach_async/poll_async/poll(다중 ctrlr 컨테이너 패턴, FIFO 보존 INSERT_HEAD 트릭). last-ref만 destruct + 다른 프로세스 사용 중이면 ref만 감소.
  - **★ admin 동기 polling 패턴**: nvme_completion_poll_cb(timed_out 경로 자동 free + cpl 복사 + done=true) ↔ nvme_wait_for_completion_poll(admin lock, poll_group vs qpair 분기, ★ PCIe CSTS all-ones link 검사로 hot-removal 감지, timed_out=true 마킹으로 늦은 callback이 스스로 free) ↔ nvme_wait_for_adminq_completion(180s timeout 변환, release 옵션).
  - **★ user_copy 패턴**: nvme_user_copy_cmd_complete + nvme_allocate_request_user_copy — 사용자 일반 메모리 + DMA-capable 사본 한 쌍 생성, CONTROLLER_TO_HOST 시 결과 복사 + PID 검증(multi-process 안전성).
  - **★★ nvme_driver_init**: g_init_mutex로 진입 직렬화 → primary는 spdk_memzone_reserve(SPDK_MEMZONE_NO_IOVA_CONTIG)로 hugepage SHARED 객체 할당 → robust mutex(PI futex 기반 PROCESS_SHARED + ROBUST) 초기화 → hotplug netlink fd + default UUID 생성 / secondary는 spdk_memzone_lookup + initialized=true까지 180s polling.
  - **★ nvme_robust_mutex_init_shared**: pthread_mutexattr_setpshared(SHARED) + setrobust(ROBUST) → holder process 사망 시 EOWNERDEAD 알림 + consistent 복구 가능. FreeBSD는 robust 미지원 → 일반 mutex로 대체.
  - **★★ nvme_ctrlr_probe + poll_internal**: 신규/기존 분기 → ref 증가 + attach_cb / 신규 construct + init_ctrlrs 추가. process_init 폴링 → 실패 destruct_async 누적 + attach_fail_cb / READY → attached 이동 + ref + attach_cb. unlock-during-cb로 사용자 콜백 안에서 detach 호출 가능.
  - **★ ABI 호환 SET_FIELD 패턴**: nvme_ctrlr_opts_init의 FIELD_OK + SET_FIELD/SET_FIELD_ARRAY 매크로 — 사용자 헤더가 구버전이어도 안전 (offset+sizeof <= opts_size 검사).
  - **★ transport_id 파싱/포맷 8종**: trtype/adrfam parse + str(대소문자 무관 입력, 표준 표기 출력), trid_populate_transport(매핑 테이블), populate_trstring(toupper 정규화 + GCC-11 LTO false positive 회피), parse_next_key(':' vs '=' 우선순위), transport_id_parse(인식/무시 키 분류), host_id_parse(같은 문자열 두 번 파싱), transport_id_compare(★ trtype 우선 + PCIe BDF 정규화 + Fabrics 4필드 순차 + subnqn case-sensitive).
  - **유틸 헬퍼**: nvme_request_check_timeout(admin/AER/KEEP_ALIVE 분기 + multi-process PID 검사), prchk_flags_parse/str(reftag/guard 4 조합), scan_attached(빈 probe_ctx + 트랜스포트 위임), nvme_parse_addr(getaddrinfo 래퍼 + gai 코드 음수 정규화), nvme_get_default_hostnqn(UUID NQN 표준 형식 "nqn.2014-08.org.nvmexpress:uuid:...").

  **결과 — NVMe 드라이버 메인 진입 메커니즘 완결**: spdk_nvme_probe()부터 controller READY까지의 전 경로 (driver_init → memzone reserve → transport scan → ctrlr_probe → process_init 상태머신 → attach_cb), multi-process hugepage 공유 driver 객체 라이프사이클, robust mutex의 PI futex crash-safe 메커니즘, admin completion 동기 polling 패턴(timed_out 자동 free 포함), user_copy 헬퍼의 DMA 사본 패턴, ABI 호환 SET_FIELD 매크로, transport_id 파싱/비교의 PCIe vs Fabrics 분기가 모두 주석만으로 추적 가능.

  **다음 세션 후보**:
  - `lib/nvme/nvme_ctrlr.c` (5997줄) — 컨트롤러 상태머신 (★★★ 매우 큼, 2-3 세션 분할 필요. process_init의 40+ 상태 전이가 핵심)
  - `lib/nvme/nvme_ns.c` — namespace identify + 설정
  - `lib/nvme/nvme_ctrlr_cmd.c` — admin 커맨드 빌더 (Identify, Set Features, Get Log Page)

- 2026-04-28 · **lib/nvme/nvme_auth.c [☑ 완료]** — **★ NVMe-oF DH-HMAC-CHAP 인증 호스트 측 완전 구현 ★**. 원본 1296줄 → 주석 후 2397줄. nvme_fabric.c가 atr/ascr 플래그 셋만 했다면, 이 파일이 그 후속 인증 핸드셰이크 전체를 8상태 비동기 머신으로 처리. NVMe-oF 1.1 스펙 Section 8.13 완전 구현. 핵심 완료:
  - **파일 상단 4섹션 블록**: DH-HMAC-CHAP의 5대 가치(PSK 비공개·DH forward secrecy·상호인증·해시 협상·8상태 머신), 두 진입 경로(자동 트리거 vs 사용자 명시), 와이어 메시지 6종 시퀀스(negotiate/challenge/reply/success1/success2/failure), OpenSSL EVP_MAC 의존성과 SPDK_CONFIG_HAVE_EVP_MAC 빌드 게이트.
  - **★ DHHC-1 키 포맷 파서 + CRC32 검증** (nvme_auth_get_key): "DHHC-1:HH:base64-key-with-crc32:" 파싱 + 36/52/68B 사이즈 검증 + spdk_crc32_ieee_update로 끝 4B CRC IEEE 검증 + spdk_memset_s로 보안 메모리 클리어.
  - **★★ 키 변환 알고리즘** (nvme_auth_transform_key): NONE 모드 raw 복사 vs HMAC(key, nqn || "NVMe-over-Fabrics") 도메인 분리 — 같은 PSK를 다른 NQN에서 쓰면 다른 키 도출 보장 (스펙 8.13.5.4).
  - **★★ DH secret nonce 보강** (nvme_auth_augment_challenge): NULL key→cval 단순 복사 vs HMAC(MD(key), cval) — DH secret으로 nonce 보강 → forward secrecy 확보 (PSK 노출돼도 과거 dhsec 모르면 풀리지 않음).
  - **★★ HMAC 응답 계산** (spdk_nvme_dhchap_calculate, 공개 API): 8 입력 누적 HMAC = caval || seq || tid || scc || type || nqn1 || NUL || nqn2. type "HostHost"/"Controller" 구분으로 양방향 인증의 도메인 분리.
  - **DH 키 관리 5종**: DHX 알고리즘 + ffdhe* RFC 7919 그룹, OSSL_PARAM_BLD로 peer key import, EVP_PKEY_dup으로 ctx 종속성 끊기, set_dh_pad(1)로 secret 길이 일관성.
  - **★ reserved_req 사용** (nvme_auth_submit_request): 일반 free_req 풀 고갈 시에도 인증 가능하도록 qpair init 시 1슬롯 격리. AUTH Send vs Recv SQE 분기 (opcode=FABRIC + fctype + spsp0/1=1 + secp=NVME).
  - **★★★ nvme_auth_send_reply 7단계 코어**: auth->hash 보존 → DH 키페어 생성 + pubkey 추출 + secret 도출 → dup된 PSK + ckey → "HostHost" rval 계산 → 상호인증 시 seqnum 채번 + RAND ctrlr_challenge 생성 + "Controller" 응답 미리 계산해 auth->challenge에 보존 → in-place dma_data 재사용으로 reply 빌드 (rval[0..hl] 호스트 응답 + rval[hl..2hl] ctrlr_challenge — cvalid=0이여도 슬롯 강제 + rval[2hl..2hl+publen] host pubkey).
  - **상호 인증 검증** (nvme_auth_check_success1): ctrlr_key 있으면 rvalid=1 강제 + hl 일치 + ★ memcmp(msg->rval, auth->challenge) — 컨트롤러가 우리가 보낸 ctrlr_challenge에 정확히 응답했는지 = 컨트롤러 신원 증명의 정점.
  - **★★★★ 8상태 머신 driver** (nvme_fabric_qpair_authenticate_poll): NEGOTIATE→AWAIT_NEGOTIATE→AWAIT_CHALLENGE→AWAIT_REPLY→AWAIT_SUCCESS1→{AWAIT_SUCCESS2 if ctrlr_key else DONE}/AWAIT_FAILURE2→DONE. **in_auth_poll 재진입 가드**, **do-while + prev_state 비교 패턴**(상태 전이 시 즉시 다음 단계 진행, 같은 상태면 -EAGAIN), NEGOTIATE 의도적 early return 이유 명시(initial kick에서 응답 없이 다음 단계 진입 방지), 각 AWAIT_*의 nvme_wait_for_completion_poll 3분기(-EAGAIN/error/0).
  - **kick-start + 동시 진행 방지**: nvme_fabric_qpair_authenticate_async가 dhchap_key/ascr 검증 + status calloc + dma_data 4KiB DMA zmalloc + tid ctrlr 단위 채번 + 첫 polling으로 NEGOTIATE 송신 트리거(-EAGAIN→0 정규화). spdk_nvme_qpair_authenticate(사용자 노출)는 EALREADY로 동시 진행 방지 후 트랜스포트 위임.

  **결과 — NVMe-oF 호스트 인증 전 메커니즘 완결**: PSK keyring → DHHC-1 파싱 → 8상태 머신 → DH 키 교환 → HMAC 응답 계산 → 상호 인증 → cleanup의 모든 단계 + 실패 분기 + 재진입 방어 + 보안 메모리 클리어 패턴이 주석만으로 완전 추적 가능. nvme_fabric.c의 atr/ascr 플래그 후속 흐름 완성.

  **다음 세션 후보**:
  - `lib/nvme/nvme.c` (2277줄) — nvme_internal.h의 nvme_complete_request 등 공용 헬퍼 구현
  - `lib/nvme/nvme_ctrlr.c` (5997줄) — 컨트롤러 상태머신 process_init/enable/disable (매우 큼, 다세션 분할)
  - `lib/nvme/nvme_ns.c` — namespace identify 및 설정

- 2026-04-28 · **lib/nvme/nvme_poll_group.c [☑ 완료]** — **★ 다중 트랜스포트 폴링 그룹 상위 추상 ★**. 원본 515줄 → 주석 후 1248줄. 이전까지 `nvme_transport.c`의 tgroup(트랜스포트별 sub-group)만 있었다면, 이 파일은 그것들을 **단일 spdk_thread/reactor에서 단일 process_completions로 묶는 최상위 컨테이너**. 핵심 완료:
  - **파일 상단 4섹션 블록**: poll group의 4가지 핵심 가치 — (1) 트랜스포트 이종 통합(PCIe + RDMA + TCP + vfio-user 단일 API), (2) accel 오프로드(`spdk_nvme_accel_fn_table`로 CRC32C/copy를 SPDK accel 프레임워크에 위임), (3) interrupt 모드 epoll 통합(`spdk_fd_group`로 모든 qpair fd + disconnect eventfd 묶음), (4) disconnected qpair 4가지 통지 경로. 호출 체인 그래프(connect 경로 + 완료 경로 6 레이어).
  - **★ spdk_nvme_poll_group_create**: ABI 호환 SET_FIELD 매크로(offset+sizeof <= table_size) + accel 콜백 일관성 2단계 검증(finish/reverse/abort XOR + append→finish 의존성) + fd_group lazy 생성 + 비-Linux fall-through.
  - **★ Linux eventfd 트리오**: `nvme_poll_group_read_disconnect_qpair_fd`(epoll 트리거 시 사용자 콜백 호출), `nvme_poll_group_write_disconnect_qpair_fd`(트랜스포트가 disconnect 발생 시 8B write로 epoll wake), `nvme_poll_group_add_disconnect_qpair_fd`(eventfd EFD_NONBLOCK|EFD_CLOEXEC 생성 + SPDK_FD_GROUP_ADD_EXT 등록 + 단일 호출 assert). 비-Linux는 stub 제공.
  - **★★ spdk_nvme_poll_group_add**: 5단계 — (1) NVME_QPAIR_DISCONNECTED 검증 (2) enable_interrupts_is_valid first-time 결정 + 이후 일관성 강제(혼용 금지) (3) STAILQ tgroup 검색 (4) 못 찾으면 nvme_get_first/next_transport로 dlopen된 트랜스포트까지 lazy-create (5) nvme_transport_poll_group_add 위임. dlopen 시나리오 지원 명시.
  - **nvme_poll_group_connect_qpair**: 트랜스포트 connect → fd 등록 → 실패 시 disconnect 롤백 패턴(정합성 유지). disconnect_qpair는 fd 제거 우선 → 트랜스포트 disconnect 순서 중요성(epoll wake로 죽은 qpair 폴링 방지).
  - **★★★ spdk_nvme_poll_group_process_completions**: ★ poll_group의 메인 폴링 진입점 ★. **`in_process_completions` 재귀 가드**(사용자 cb_fn 안에서 재호출 차단), `error_reason` 첫 음수 보존 + `num_completions` 양수 누적 정책, `spdk_unlikely`로 hot-path 분기 최적화. 호출 체인 6 레이어 그래프 문서화.
  - **spdk_nvme_poll_group_all_connected**: -EIO 즉시 반환(disconnected 또는 CONNECTING 미만) vs -EAGAIN 보류(CONNECTING) vs 0(모두 OK). tgroup 단위 early break 최적화.
  - **★ spdk_nvme_poll_group_destroy**: STAILQ_FOREACH_SAFE + REMOVE 후 destroy 실패 시 INSERT_TAIL 롤백 + EBUSY 패턴, fd_group 해제 시 disconnect_qpair_fd remove → close → fd_group destroy 순서.
  - **★ get_stats**: 2단계 순회(1차 카운트 + 2차 수집), 트랜스포트별 부분 실패 허용(reported_stats_count), 모두 실패 시 -ENOTSUP. **free_stats**: trtype 매칭 + freed_stats == num_transports assert 검증.

  **결과**: poll group이 **reactor 1코어 = 1 인스턴스 + N 트랜스포트 sub-group + N qpair**를 단일 process_completions로 묶는 메커니즘, polling vs interrupt 모드의 fd_group 통합 경로(특히 SPDK_FD_TYPE_EVENTFD로 자동 read 위임), dlopen된 트랜스포트의 lazy tgroup 생성, ABI 호환을 위한 SPDK_SIZEOF/SET_FIELD 패턴, disconnected qpair 통지 4가지 경로(즉시 wait 진입 시 / 정기 process_completions / interrupt eventfd / 사용자 callback)가 모두 주석만으로 추적 가능.

  **다음 세션 후보**:
  - `lib/nvme/nvme_auth.c` (1296줄) — DH-CHAP 인증 상태머신 (nvme_fabric.c의 atr/ascr 후속 흐름)
  - `lib/nvme/nvme.c` (2277줄) — nvme_complete_request 등 공용 헬퍼 구현
  - `lib/nvme/nvme_ctrlr.c` (5997줄) — 컨트롤러 상태머신 (매우 큼, 섹션 분할)

- 2026-04-26 · **lib/nvme/nvme_fabric.c [☑ 완료]** — **★ NVMe-oF 트랜스포트 공통 레이어 완전 정복 ★**. 원본 671줄 → 주석 후 1578줄. PCIe NVMe와 NVMe-oF의 본질적 차이를 드러내는 파일. PCIe는 호스트가 BAR을 mmap하여 CC/CSTS/AQA/ASQ/ACQ 레지스터를 직접 MMIO로 R/W하지만 RDMA/TCP/FC/vfio-user에서는 같은 레지스터 R/W를 **Fabric Property Set/Get 커맨드(opcode 0x7F + fctype 0x00/0x04)**로 메시지 변환해야 함 — 이 파일이 그 변환 계층. 핵심 완료:
  - **★ Property R/W 6종 헬퍼 + 8종 공개 API**: prop_set_cmd/sync/done/async + prop_get 동일 패턴. SQE 빌드(opcode=0x7F, fctype, ofst, attrib.size, value.u64), value union으로 4B/8B 양쪽 지원, sync는 nvme_completion_poll_cb + busy-wait, async는 nvme_fabric_prop_ctx 트램펄린(spdk_nvme_cmd_cb → spdk_nvme_reg_cb 시그니처 변환). robust=true/false의 status leak 정책 차이 명시.
  - **★ Discovery 3종**: discover_probe(subtype 검사 — DISCOVERY/CURRENT는 referral skip + NVME만 처리, trtype available_by_name 검사, NQN/traddr/trsvcid 정규화 — strlen_pad/str_chomp/null-terminate, priority 상속, nvme_ctrlr_probe 재귀), get_discovery_log_page(LID=0x70 GET LOG PAGE 동기), ctrlr_discover(4KB 버퍼 페이징 알고리즘 — 첫 페이지 헤더 16B + 3 entries vs 이후 페이지 4 entries, recfmt=0 검증, numrec/log_page_offset 누적 갱신).
  - **★ ctrlr_scan**: subnqn != DISCOVERY_NQN → 직접 nvme_ctrlr_probe / DISCOVERY_NQN → 임시 discovery_ctrlr 생성(default opts) → process_init 루프로 READY까지 → Identify Controller로 cdata → direct_connect 분기(true=discovery_ctrlr 자체를 attach + add_process / false=ctrlr_discover 실행 후 destruct).
  - **★★ qpair_connect_async** — NVMe-oF CONNECT 핸드셰이크의 시작점. 9단계 시퀀스: 인자 검증(num_entries 0~MAX) → DMA 1024B nvmf_data spdk_zmalloc(SPDK_MALLOC_DMA) → status tracker calloc + dma_data 보존 → SQE 빌드(opcode=0x7F + fctype=0x01 + qid + sqsize=N-1(0-based) + kato) → **reserved_req 사용**(일반 free_req 풀이 비어 있어도 CONNECT/AUTH는 무조건 가능해야 하므로 큐페어 init 시 1슬롯 격리 보존) → NVME_INIT_REQUEST(콜백/페이로드/길이) + memcpy(cmd) → 페이로드 채우기(admin=cntlid 0xFFFF / I/O=ctrlr->cntlid + hostid 16B + hostnqn + subnqn) → nvme_qpair_submit_request → timeout_tsc 계산 + auth.flags 0 클리어 + fabric_poll_status 저장.
  - **★ qpair_connect_poll** — 응답 폴링 + 결과 추출. nvme_wait_for_completion_poll → -EAGAIN이면 재호출, 도착하면 timed_out/cpl_is_error 분기(timed_out → -ECANCELED + 진단 로그(trtype/adrfam/traddr/trsvcid/subnqn) / sct,sc 에러 → -EIO + sct/sc 로깅). SUCCESS는 status_code_specific.success union 파싱: admin이면 cntlid 추출 → ctrlr->cntlid 영구 저장(이후 I/O CONNECT의 페이로드에 첨부되는 controller association 키), authreq.atr=1 → qpair->auth.flags.atr=1, ascr=1 → qpair->auth.flags.ascr=1.
  - **수명주기 정리**: poll_cleanup(timed_out=true면 leak 허용 — late callback에서 use-after-free 회피), auth_cleanup(idempotent — cb_fn=NULL 후 두 번 호출 방어).
  - **★ qpair_auth_required**: 4-조건 OR — atr || ascr || dhchap_ctrlr_key != NULL || auth.cb_fn != NULL. 컨트롤러 요구·호스트 정책·사용자 명시 4가지 경로 통합.
  - **qpair_connect**: connect_async + busy-wait { connect_poll } 동기 래퍼.

  **결과 — NVMe-oF 진입점 완비**: 이 파일을 통해 (1) PCIe vs NVMe-oF의 레지스터 R/W 추상화 차이, (2) Discovery 서비스가 어떻게 다단계 페이징으로 entry를 가져오는지, (3) NVMe-oF CONNECT 와이어 포맷 1024B(hostid·cntlid·subnqn·hostnqn) + admin=0xFFFF의 controller 할당 메커니즘, (4) 인증 진입 결정 정책 4가지가 모두 주석만으로 추적 가능. 다음 세션은 nvme_auth.c (DH-CHAP 인증 상태머신) 또는 nvme_poll_group.c (poll group 상위 추상) 중 선택.

- 2026-04-25 · **lib/nvme/nvme_pcie.c [☑ 완료]** — **★ PCIe 트랜스포트 cold-path 완전 정복 ★**. 원본 1173줄 → 주석 후 1920줄. 파일 상단 4섹션 블록에서 PCIe 5대 책임(probe/BAR/MMIO/SIGBUS/CMB-PMR) + 호출 체인 + SPDK_NVME_TRANSPORT_REGISTER 등록 원리 문서화. 핵심 완료:
  - **★ SIGBUS 방어** (link loss 시 BAR을 anonymous MAP_FIXED remap + 0xFF 채움, atomic CAS 재진입 방지, g_thread_mmio_ctrlr TLS 마커로 어느 컨트롤러인지 식별)
  - **★ MMIO 레지스터 R/W** (set/get_reg_4/8 + ASQ/ACQ/AQA/CMB*/PMR* wrappers, all-ones 감지로 link 무효 탐지)
  - **★ CMB 5종** (CAP.CMBS + CMBSZ SZU/SZ/SQS + CMBLOC BIR/OFST 해석 + BAR mmap + 2MB 정렬 DPDK 등록)
  - **★ PMR 6종** (CMSS 지원 시 PMRMSCU/L로 CBA 설정 + CBAI 검증 + config_pmr의 PMRTO/PMRTU timeout 대기)
  - **★★ allocate_bars** (BAR0 mmap + doorbell_base 계산 — nvme_pcie_common.c의 sq_tdbl/cq_hdbl 기반)
  - **★★ ctrlr_construct** (pci_claim → 구조체 할당 → quirks/NUMA → allocate_bars → PCI CMD 0x404 busmaster+INTx disable → doorbell_stride 계산 → admin qpair → 프로세스 등록 → 최초 시 SIGBUS 핸들러)
  - **★ ctrlr_scan/pcie_nvme_enum_cb** (DPDK enumerate 경유 primary/secondary 분기 + traddr 필터)
  - **★ ctrlr_enable** (ASQ/ACQ/AQA 레지스터 세팅)
  - **★★★ pcie_ops vtable** (42개 함수 포인터 — 이 파일 cold-path + nvme_pcie_common.c hot-path 전체 바인딩)
  - **★★★ SPDK_NVME_TRANSPORT_REGISTER(pcie, &pcie_ops)** (constructor로 main() 전 자동 등록)

  **이로써 SPDK NVMe 드라이버의 다섯 기둥 완성**: (1)☑ nvme_ns_cmd.c (2)☑ nvme_qpair.c (3)☑ nvme_transport.c (4)☑ nvme_pcie_common.c (5)☑ nvme_pcie.c. 애플리케이션 spdk_nvme_probe()부터 실제 장치 DMA까지 전 여정이 주석만으로 완전 추적 가능.

- 2026-04-24 · **lib/nvme/nvme_pcie_common.c [☑ 완료]** — **★ PCIe hot-path 구현 완전 정복 ★**. 원본 1912줄 → 주석 후 3313줄. 이번 세션에서 잔여 inline 주석 완료: `build_contig_hw_sgl_request`(CONTIG→SGL vtophys 물리 segment 분해 루프), `build_hw_sgl_request`(SGL→SGL 전체 본문 - next_sge_fn/Bit Bucket/merge 최적화/내부 루프), `build_prps_sgl_request`(SGL→PRP 누적 호출 + 페이지 경계 assert 설명). 이전 세션의 핵심부와 합쳐 파일 전체 완결. 파일 상단 4섹션 블록에서 SPDK NVMe의 "1 IOPS가 어떻게 처리되는지" 전 과정 문서화 — **submit: 사용자 API → _nvme_ns_cmd_rw → nvme_qpair_submit_request → nvme_transport_qpair_submit_request → ★ nvme_pcie_qpair_submit_request → tracker 할당 → PRP/SGL 빌드 → ★ submit_tracker → SQ[sq_tail]=SQE + doorbell MMIO write → 장치 fetch**. **complete: 장치가 CQE 기록 + phase 토글 → reactor poller → process_completions → phase bit 검사 → tracker 복원 → complete_tracker → nvme_complete_request → cb_fn**. 핵심 함수 10개 모두 ★★★ 상세 주석: submit_request(5단계 dispatch), process_completions(7단계 + next prefetch + memory barrier), submit_tracker(SSE2 non-temporal SQE copy + doorbell), complete_tracker(retry+multi-process), prp_list_append(PRP1/PRP2/list 3-mode + 4KB 경계), build_metadata(SGL_MPTR_SGL vs CONTIG MPTR), qpair_construct(tracker 풀 레이아웃), connect 체인(Create CQ→SQ 비동기 콜백 + shadow doorbell setup), delete_io_qpair(2단계 Delete SQ→CQ), copy_command(QEMU MMIO + SSE2 hot variants). 잔여: build_hw_sgl_request/build_prps_sgl_request 내부 루프 inline 세부.

- 2026-04-24 · **lib/nvme/nvme_transport.c [☑ 완료]** — **★ 트랜스포트 vtable 디스패치 레이어 ★**. 원본 976줄 → 주석 후 1820줄. 상단 4섹션 블록에서 SPDK NVMe의 vtable 아키텍처 전체 구조를 문서화: 트랜스포트(PCIe/RDMA/TCP/VFIOUSER/CUSE)별 ops 구조체가 `SPDK_NVME_TRANSPORT_REGISTER` 매크로로 전역 TAILQ에 등록되며, 상위 레이어는 이 파일의 `nvme_transport_*` 래퍼만 호출. 핵심 설계 이슈: **multi-process 안전성**(primary/secondary 프로세스가 각자 g_transports[]에 다른 주소의 ops를 가지므로 admin 큐는 매번 `nvme_get_transport(trstring)` 조회 필요, I/O 큐만 `qpair->transport` 캐시 사용). **async fallback 패턴**: set/get_reg_4/8_async가 트랜스포트별 async 미구현 시 동기 호출 + `register_operations` STAILQ에 가짜 완료 큐잉 → 다음 admin `process_completions`가 `nvme_complete_register_operations`로 처리 (nvme_qpair.c 완성된 이전 세션 작업과 짝). **Hot-path 5종 공통 패턴**(abort_reqs/reset/submit_request/process_completions/iterate_requests): `spdk_likely(!is_admin) → qpair->transport->ops.xxx` vs `nvme_get_transport→ops.xxx`. 기타 완료: 레지스트리 5종, 컨트롤러 수명주기 7종, 동기 레지스터 4종, **CMB 3종** + **PMR 4종**(장치 내부 메모리 활용, optional 지원 차이 -ENOTSUP/-ENOSYS), qpair 관리 8종(connect_qpair의 동기/비동기 busy-wait, disconnect_qpair_done의 active_proc 기반 abort), 인증·AER abort, **Poll Group 10종**(transport tgroup + connected/disconnected STAILQ 이동 불변식 + num_connected_qpairs 카운터), ABI 호환 opts SET_FIELD 매크로, volatile register 포인터 공개. **이 파일만 읽어도 SPDK NVMe 드라이버의 vtable 디스패치 아키텍처·멀티프로세스 안전성·hot/cold 경계·optional op 규약 전체가 파악 가능**.

- 2026-04-24 · **lib/nvme/nvme_qpair.c [☑ 완료]** — **★ Queue Pair 제출/완료/상태머신 코어 ★**. 원본 1314줄 → 주석 후 2371줄. 파일 상단 4섹션 블록(submit/complete 호출 그래프 + state machine 다이어그램 + 공유 자료구조 맵) + 전역 opcode/status 사전 11종(admin/fabric/feat/io/sgl_type/sgl_subtype/status_type/generic/cmd_specific/media_error/path 각 테이블과 sentinel 규약) + 디버그 프린터 11개(PRP/SGL/DPTR/Admin/IO cmd·completion 문자열화) + state_string(수명주기 아스키 다이어그램) + is_retry(DNR 기반 TP 4028 정책) + **★ manual_complete_request**(가짜 CQE 합성 후 cb_fn 호출) + abort_queued/SWAP-기반 재귀 회피 + abort_queued_with_cbarg/격리 리스트 + **★ check_enabled**(CONNECTED→ENABLING→ENABLED 전이, PCIe reset abort, queued_req flush, reset 감지→disconnect) + **★ resubmit_requests**(완료 개수만큼 flow control 재제출) + **★ complete_register_operations**(multi-process register 완료 큐) + **★★★ spdk_nvme_qpair_process_completions**(7단계 상세: admin 선행 → is_failed 체크 → check_enabled → error injection → transport delegate → delete_after_completion → resubmit) + getter 6종 + **★ init/deinit**(req_buf 풀 64B align, reserved_req 특수 슬롯) + **★★★ _nvme_qpair_submit_request**(9단계: state check → split children 재귀 → err injection → submit_tick → ENABLED/FABRIC-CONNECTING 허용 조건 → transport submit → EAGAIN/error 경로) + **nvme_qpair_submit_request / resubmit_request / abort_all_queued_reqs**(FIFO 순서 유지 + queued_req 큐잉 규약) + **add/remove_cmd_error_injection**(admin lock 규약) 전부 주석.

  **결과**: 이 파일만 읽으면 SPDK NVMe의 **호스트→장치 submit** 경로, **장치→호스트 complete** 경로, **reset 중 transition**, **split parent/child 제출 규약**, **error injection 메커니즘**, **multi-process register 완료 큐**, **FIFO 순서 보존 vs FABRIC 우선순위** 모두 주석만으로 완전 추적 가능. 특히 `_nvme_qpair_submit_request`의 9단계와 `process_completions`의 7단계는 NVMe 드라이버 이해의 정점.

- 2026-04-24 · **lib/nvme/nvme_ns_cmd.c [☑ 완료]** — **★ I/O 경로 구현 파일 첫 완전 정복 ★**. 원본 1516줄 → 주석 후 2592줄. 이전 세션의 핵심(setup_request, _nvme_ns_cmd_rw, spdk_nvme_ns_cmd_read/write)에 이어 이번 세션에서 **29개 공개 API 나머지 전부 주석**:
  - **Compare 4종**(compare/compare_with_md/comparev/comparev_with_md): fused CAS 용도 설명, PI apptag/mask 동작, SGL vs CONTIG 경로 분기
  - **Read/Write 변형 6종**(read_with_md/readv/readv_with_md/readv_ext, write_with_md/writev/writev_with_md/writev_ext + write_ext/read_ext): separate metadata 경로, ZCL/ext_io_opts 사용, accel_sequence 오프로드 조건
  - **_ext 내부 빌더 2종**(nvme_ns_cmd_rw_ext — CONTIG, nvme_ns_cmd_rwv_ext — SGL): opts NULL 처리, accel 검증, payload.opts 전파
  - **Zone Append 3종**(check_zone_append + zone_append_with_md + zone_appendv_with_md): ZNS wp 자동 할당 개념, ZASL ≤ MDTS 조기 검사, split 금지 방어 로직, get_append_location 완료 CQE에서 최종 LBA 반환
  - **관리형 I/O 6종**(write_zeroes/verify/write_uncorrectable/DSM/copy-SCC/flush): payload-없는 request 빌드(nvme_allocate_request_null), user_copy 버퍼 이동, 각 SQE CDW 필드 매핑, 사용처(discard/scrub/VWC 플러시)
  - **Reservation 4종**(register/release/acquire/report): multi-host PR 메커니즘 설명, RREGA/RRELA/RACQA/RTYPE 값별 의미, 128-bit Host ID와 EDS 플래그 분기
  - **IO Management 2종**(recv/send): NVMe 2.0 TP4100 관리 서브커맨드 프레임워크, MO/MOS 필드, FDP 사용처

이로써 **lib/nvme/nvme_ns_cmd.c의 모든 함수·실행 라인·SQE 필드 매핑이 주석만으로 완전 추적 가능**. 이 파일 하나로 애플리케이션 → _nvme_ns_cmd_rw → SQE → submit 전 여정이 이해됨.

- 2026-04-22 · **lib/nvme/nvme_ns_cmd.c [부분 ◐ 핵심 완료]** — **★ I/O 경로 구현 파일 첫 진입 ★**. 파일 상단 4섹션 블록(I/O 경로 번역 레이어 위치, 전체 호출 흐름 상세, 지원 명령 목록, 상위·하위 연결) + 보조 함수 10종(check_request_length / map_failure_rc / md_excluded_from_xfer / get_host_buffer_sector_size / get_sectors_per_max_io / add_child_request / split_request / is_io_flags_valid / is_accel_sequence_valid) + **_nvme_ns_cmd_setup_request**(★ SQE CDW10-15 필드 매핑: SLBA 64bit alias / PI Type1·2 RefTag / fused / NLB 0-based / AppTag+mask 비트 배치) + **_nvme_ns_cmd_rw**(★ R/W 공통 엔트리: nvme_allocate_request → stripe/MDTS/SGL/PRP split 판정 → setup_request) + **spdk_nvme_ns_cmd_read + spdk_nvme_ns_cmd_write**(★ 공개 API 진입점, 호스트→장치 전체 SQE 제출 순서 문서화) 전부 상세 주석.
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

**2026-04-29 (서른세 번째 파트 — 4차 병렬 agent: nvme_tcp + JSON/RPC + trace/dma)**: 3개 병렬 agent로 7개 파일 일괄 처리. lib/nvme의 큰 파일(nvme_tcp.c) 단독 + include/spdk 헤더 6개를 두 묶음(JSON/RPC + trace/dma)으로 분배.

원본 5391줄 → 주석 후 9102줄 (+3711줄), 약 130개 함수/매크로/구조체 보강.

병렬 agent 분배:
1. **agent 1**: nvme_tcp.c (3416→4813, +1397) — 50개 함수 신규 보강 (이전 시도에서 4섹션 블록 + 구조체는 이미 있었음, 이번에 모든 함수/실행 라인 완성). PDU 송신 10/PDU 수신 10/handshake 5/R2T flow 2/TermReq 3/qpair 라이프사이클 8/TLS PSK 1/poll group 10/vtable 기타 10.
2. **agent 2**: JSON/RPC 헤더 3개 — json.h(353→843), jsonrpc.h(357→715), rpc.h(156→363). 매크로/enum/구조체/함수 모두.
3. **agent 3**: trace/dma 헤더 3개 — trace.h(506→1153), trace_parser.h(131→314), dma.h(472→901).

**핵심 패턴 추가**:
- ★ NVMe-oF TCP PDU 인코딩: CapsuleCmd plen/pdo/HDGST/DDGST 레이아웃, padding 계산, in-capsule data 결정(ioccsz)
- ★ R2T flow control: maxr2t, ttag, datao 검증, 직전 R2T 보관 케이스
- ★ HDGST/DDGST CRC32C: accel framework 가속 vs SW fallback, recv 시 op 역순 (reverse_sequence)
- ★ TLS PSK HKDF 유도: identity/retained/TLS PSK 흐름 (TP 8011)
- ★ TCP recv 상태머신: CH→PSH→PAYLOAD→QUIESCING + accel_recv_* 분기
- ★ JSON-RPC 2.0 표준: server/client + Parse/InvalidRequest/MethodNotFound/InvalidParams/InternalError 6 에러 코드
- ★ SPDK_RPC_REGISTER constructor 자동 등록: priority 1000(method)/1001(alias), STARTUP/RUNTIME phase gating, allowlist
- ★ Trace lockless circular buffer: per-lcore history → /dev/shm 매핑 → spdk_trace CLI 후처리, MAX_LCORE/OWNER_TYPE/OBJECT/GROUP_ID/TPOINT_ID 분류
- ★ Trace argument encoding: ARG_TYPE_INT/PTR/STR + MAX_ARGS_COUNT preprocessor 트릭
- ★ DMA memory domain: spdk_memory_domain_translate_data — host RAM ↔ GPU memory ↔ RDMA registered ↔ NVMe CMB 변환, accel_sequence와 zero-copy I/O 통합

**lib/nvme 디렉토리 진행 현황**: 잔여 큰 파일 거의 마무리.
- ◐ `nvme_ctrlr.c` (5997줄) — Part 3 잔여 ~90 함수
- ☐ `nvme_rdma.c` (4079줄) — RDMA 트랜스포트, **다음 라운드 단독 agent**

**include/spdk 진행 현황**: 핵심 RPC/trace/DMA 모두 완료. 잔여:
- ☐ `tree.h` (842줄, BSD tree 매크로 — 전용 세션)
- ☐ `dif.h` (이미 완료)

다음 세션 후보:
- **nvme_rdma.c (4079줄) — 단독 agent 권장** (한도 분산)
- nvme_ctrlr.c Part 3 (계속)
- include/spdk 잔여 + lib/thread/thread.c

---

**2026-04-28 (서른두 번째 파트 — 3차 병렬 agent: include 헤더 + bdev 작은 파일)**: 이전 시도에서 한도 초과로 실패한 작업 재시도 성공. 2개 병렬 agent로 7개 파일 처리.

원본 2375줄 → 주석 후 5153줄 (+2778줄).

병렬 agent 분배:
1. **agent 1**: include/spdk 헤더 3개 — log.h(이미 완료 검증), histogram_data.h(286→609, 11종+7매크로), dif.h(492→999, 22종+9매크로+3enum+3구조체)
2. **agent 2**: lib/bdev 작은 파일 4개 — bdev_zone.c(이미 완료 검증, 13종), part.c(691→1481, 22종+3구조체), scsi_nvme.c(234→483, 1종), vtune.c(이미 완료 검증)

**핵심 패턴 추가**:
- ★ T10 DIF/DIX 보호정보 — Guard CRC + AppTag + RefTag, NVMe PI Type 1/2/3 + 16/32/64bit format, DIF(인터리브) vs DIX(분리메타) 두 모드 + stream API + dif_remap_ref_tag
- ★ histogram logarithmic range × linear bucket — TSC delta 누적, per-thread + merge로 lockless 집계, P50/P99 산출
- ★ partition vbdev 공통 코어 — offset+length로 base bdev 분할, DIF Reference Tag remap (LBA 변환 시 reftag도 변환), thread affinity (base->thread 보존)
- ★ SCSI sense ↔ NVMe completion 양방향 변환 — SCT 4종(GENERIC/COMMAND_SPECIFIC/MEDIA_ERROR/VENDOR_SPECIFIC) 1차 분기 + SC 2차 분기, iSCSI/vhost-scsi target에서 NVMe backend 사용 시 어댑터

**lib/nvme 디렉토리 진행 현황**: 거의 모든 파일 완료. 잔여 큰 파일:
- ◐ `nvme_ctrlr.c` (5997줄) — Part 3 잔여 ~90 함수
- ☐ `nvme_rdma.c` (3742줄) — RDMA 트랜스포트
- ☐ `nvme_tcp.c` (3077줄) — TCP 트랜스포트

**lib/bdev 진행 현황**: 작은 파일 모두 완료, 잔여:
- ☐ `bdev.c` (11524줄) — bdev 코어, 매우 큼
- ☐ `bdev_rpc.c` (1233줄)

**include/spdk 진행 현황**: 자주 쓰이는 헤더 거의 완료, 잔여:
- ☐ `tree.h` (842줄, BSD tree 매크로)
- ☐ `json.h`, `jsonrpc.h`, `rpc.h`
- ☐ `trace.h`, `trace_parser.h`

다음 세션 후보:
- nvme_tcp.c (3077줄) — 단독 agent
- nvme_rdma.c (3742줄) — 단독 agent
- nvme_ctrlr.c Part 3 (계속)
- include/spdk/json.h + jsonrpc.h + rpc.h 묶음

---

**2026-04-28 (서른한 번째 파트 — 2차 병렬 agent 7개 파일 일괄 완료)**: 병렬 agent 4개 동시 실행하여 SPDK NVMe lib/nvme/ 디렉토리의 미작업 파일을 추가 완료. 누적 통계: 원본 5548줄 → 주석 후 8712줄 (+3164줄), 135개 함수 보강 + 1개 헤더 파일(nvme_pcie_internal.h) 이미 완료 상태 검증.

병렬 agent 분배:
1. **agent 1**: 헤더+stubs 묶음 — nvme_pcie_internal.h(712줄, 검증) + nvme_stubs.c(77→261, 9종) + nvme_ctrlr_ocssd_cmd.c(69→219, 2종)
2. **agent 2**: 작은 파일 묶음 — nvme_ns_ocssd_cmd.c(205→484, 6종) + nvme_vfio_user.c(361→874, 14종+vtable)
3. **agent 3**: nvme_cuse.c(1556→2559, +1003줄, 54종) — CUSE/libfuse3 통합
4. **agent 4**: nvme_opal.c(2568→4315, +1747줄, 50종) — TCG Opal SED 클라이언트

**핵심 검증된 패턴들**:
- ★★ TCG Opal SSC 2.0 session 4단계 핸드셰이크 (Discovery SECP_INFO → StartSession HSN/TSN 발급 → 메서드 호출 CALL <obj_UID> <method_UID> → EndSession EOS 토큰)
- ★★ TCG SWG TLV 토큰 빌더 7종 (u8/u64/bytestring/short atom/medium atom/long atom/finalize) + 응답 파서 12종 (tiny/short/medium/long token + status)
- ★ TakeOwnership 핸드셰이크 (ANYBODY 세션 → MSID GET → SID 세션 → C_PIN_SID Set + PIN 메모리 즉시 0 클리어)
- ★ CUSE/libfuse3 char device 시뮬레이션 — 단일 CUSE thread (do-while + spdk_fd_group_wait 500ms + eventfd 통지) + nvme_io_msg.c 채널 통한 SPDK reactor 위임 → ioctl(ADMIN_CMD/IO_CMD/SUBMIT_IO/RESET/RESCAN/BLK*GET) 변환
- ★ vfio-user 트랜스포트 — BAR/Config을 socket RPC로 외부화, control-plane vfio-고유, hot-path는 nvme_pcie_* 재사용 (vtable로 cold-path만 덮어씀)
- ★ OCSSD vector I/O — opcode 0x90~0x93, num_lbas==1 vs 다중 인라인 LBA 최적화, cdw12=num_lbas-1 0-based 인코딩
- ★ SPDK_CONFIG 비활성 시 stub fallback — -ENOTSUP 정책 vs RDMA fail-fast(abort()) 정책 분기
- ★ NVME_QUIRK_OCSSD + CNEX Labs vid + ns vendor_specific[0]==0x1 휴리스틱 — Open-Channel SSD 감지

모든 파일에 4섹션 블록 정확히 포함 (grep 검증 4/4 sections).

**lib/nvme/ 디렉토리 진행 현황**: 작은~중간 파일 거의 모두 완료. 주요 미작업:
- ◐ `nvme_ctrlr.c` (5997줄) — Part 3 잔여 ~90 함수
- ☐ `nvme_rdma.c` (3742줄) — RDMA 트랜스포트, 매우 큼
- ☐ `nvme_tcp.c` (3077줄) — TCP 트랜스포트, 매우 큼

다음 세션 후보: nvme_ctrlr.c Part 3 (Features/Reset/AER 우선) → nvme_tcp.c → nvme_rdma.c.

---

**2026-04-28 (서른 번째 파트 — 병렬 agent 6개 파일 일괄 완료)**: 사용자 요청으로 4개 병렬 agent 동시 실행하여 SPDK NVMe 6개 파일을 한 세션에 완료. 누적 통계: 원본 ~3623줄 → 주석 후 ~5184줄 (+1561줄), 61개 함수 보강 + 1개 파일(nvme_ns.c) 이미 완료 상태 검증.

병렬 agent 분배:
1. **agent 1**: nvme_ns.c (1582줄) — 결과: 이미 표준 양식으로 완료된 파일임을 검증, 추가 보강 불필요
2. **agent 2**: nvme_ctrlr_cmd.c (1048→1682줄, +634) — 28개 admin 커맨드 빌더 + 7개 abort 보조. 각 함수의 NVMe spec opcode (0x06/0x09/0x0A/0x0D/0x10/0x11/0x15/0x19/0x1A/0x80/0x81/0x82/0x84/0x7C 등) + CDW10-15 비트필드 매핑 상세 (IDENTIFY의 CNS/CNTID/CSI, Get Log Page의 NUMDL/NUMDU/LPOL/LPOU/LID, Abort의 SQID/CID, Format의 LBAF/MS/PI/PIL/SES, Sanitize의 SANACT/AUSE/OWPASS, Security의 SECP/SPSP0/SPSP1/NSSF, Doorbell Buffer의 PRP1/PRP2)
3. **agent 3**: nvme_io_msg.c (217→752줄, +535, 7종) + nvme_util.c (191→526줄, +335, 3종) — 외부 non-SPDK 스레드 메시지 채널 (MP-SC ring + 전용 io_qpair + producer STAILQ) + CLI 옵션 공용 유틸
4. **agent 4**: nvme_quirks.c (163→392줄, +229, 2종+테이블) + nvme_zns.c (255→678줄, +423, 17종) + nvme_discovery.c (167→472줄, +305, 4종) — PCI ID quirk 테이블 + ZNS TP 4053 + Discovery Log Page

**핵심 검증된 패턴들** (각 파일에 깊이 있게 주석 추가):
- ★ admin 커맨드의 lock→allocate→fill→submit 패턴 (28개 함수 모두 동일)
- ★ abort fan-out (parent/child + ACL 처리 흐름)
- ★ 외부 thread → ring put → SPDK reactor가 process_io_msgs로 dequeue 패턴 (SPDK lockless/affinity 우회)
- ★ Discovery 3단계 콜백 체인 (header→full page→genctr 재조회) + atomic snapshot 보장 (start/end_genctr 비교 → 변동 시 재시작)
- ★ ZNS opcode 0x7D (Zone Append) / 0x7A (Mgmt Receive) / 0x79 (Mgmt Send) + ZASL + lbafe/mor/mar 0-based 인코딩 + zone state machine
- ★ Quirk PCI ID 5튜플 룩업 + 와일드카드 + PRINT_QUIRK 디버그 매크로
- ★ trid 파싱 ns/hostnqn/alt_traddr 확장 키 + spdk_pci_device_get_id 통한 build_name

모든 파일에 4섹션 블록 (파일의 역할 / 전체 아키텍처에서의 위치 / 타 모듈과의 연결 / 주요 함수/구조체 요약) 정확한 제목·순서로 포함되어 있음을 grep으로 검증 (4/4 sections).

다음 세션 후보:
- nvme_ctrlr.c Part 3 (features/reset/AER/multi-process/sub-callbacks/public APIs) — 단일 파일 잔여 ~90 함수
- nvme_pcie_internal.h, nvme_cuse.c, nvme_opal.c, nvme_ctrlr_ocssd_cmd.c — 미작업 lib/nvme 파일들

---

**2026-04-28 (스물아홉 번째 파트 — lib/nvme/nvme_ctrlr.c Part 2: qpair 관리 ◐)**: 5997줄 거대 파일의 두 번째 분할 세션. qpair 라이프사이클 사용자 API 10종 보강.

원본 6265줄 → 6510줄 (+245줄). v1 핵심 8종에 이어 이번 세션에서 추가:

- **★ multi-process qpair 등록 패턴** (nvme_ctrlr_proc_add_io_qpair): alloc된 qpair를 현 PID의 active_procs[] 중 일치하는 process 객체에 등록. back-pointer(qpair->active_proc)도 설정. **핵심**: 같은 controller에 여러 프로세스가 attach 가능하므로 각 프로세스는 자기 qpair만 추적 — 프로세스 종료 시 일괄 정리에 사용.

- **★ ABI 호환 SET_FIELD 패턴 + STATIC_ASSERT 가드** (get_default_io_qpair_opts + opts_copy): 13개 필드(qprio/io_queue_size/requests/delay_cmd_submit/sq.vaddr/paddr/buffer_size×2/create_only/async_mode/disable_pcie_sgl_merge) 모두 FIELD_OK + SET_FIELD로 ABI 안전. SPDK_STATIC_ASSERT(sizeof==80B)로 새 필드 추가 시 컴파일 가드.

- **★ qprio + AMS 검증 정책** (nvme_ctrlr_create_io_qpair): qprio 비트는 SPDK_NVME_CREATE_IO_SQ_QPRIO_MASK 안에만 (URGENT/HIGH/MEDIUM/LOW 4종). CC.AMS=RR(Round Robin)이면 qprio=URGENT 강제 — RR은 모든 큐 동일 가중치, qprio 무의미 (스펙 정의).

- **★★ alloc_io_qpair의 8단계 + 사용자 SQ/CQ 버퍼 검증** (spdk_nvme_ctrlr_alloc_io_qpair): state==READY 검증(reset/init 중에는 free_io_qids bitmap 없음) → 기본 opts + 사용자 overlay → ★ 사용자 sq.vaddr 제공 시 buffer_size >= io_queue_size × 64B(SQE) 검증, cq.vaddr 시 × 16B(CQE) 검증 → interrupt + delay_cmd_submit 충돌 거부 → create_io_qpair → create_only 분기 → 자동 connect → 실패 시 4단계 cleanup (proc_remove + tailq remove + qid bitmap set + transport delete).

- **★ reconnect 상태 4분기** (spdk_nvme_ctrlr_reconnect_io_qpair): is_removed → -ENODEV (영구 불가능, 사용자 폐기), is_resetting/DISCONNECTING → -EAGAIN (잠시 후 재시도), is_failed/DESTROYING → -ENXIO (회복 불가), 이미 connected → 0 (idempotent), DISCONNECTED → transport connect (실패 시 -EAGAIN 정규화). 각 음수 코드는 사용자에게 다른 처리 결정 신호.

- **★★ free_io_qpair의 ★ in-completion 자기 free 패턴 ★** (spdk_nvme_ctrlr_free_io_qpair): qpair->in_completion_context 검사 — 사용자가 process_completions의 cb_fn 안에서 free 호출 시 cb_fn이 qpair 사용 중이라 즉시 free 못 함. **delete_after_completion_context=1 마킹 후 return** → process_completions가 모든 callback 끝나고 이 플래그 검사하여 실제 free 진행. 이 패턴 덕분에 사용자가 안심하고 cb_fn 안에서 자기 자신 qpair free 호출 가능.

- **★ async disconnect 폴링 + DISCONNECTING 대기**: nvme_transport_ctrlr_disconnect_qpair는 트랜스포트별 sync 또는 async. async면 DISCONNECTING 상태로 진입 후 process_completions로 진행. while 루프로 DISCONNECTED 도달까지 폴링 — poll_group_remove가 DISCONNECTED 상태 요구하므로 leak 방지.

- **★ foreign qpair 안전성** (multi-process): qpair->active_proc != current_process이면 abort_all_queued_reqs 스킵. callback도 그 프로세스 컨텍스트라 우리가 호출하면 cb_arg 잘못된 메모리 참조 → UAF 위험. 이 검사 덕분에 한 프로세스가 비정상 종료 시 다른 프로세스가 안전하게 정리 가능.

**결과 — qpair 라이프사이클 완결**: 사용자가 spdk_nvme_ctrlr_alloc_io_qpair → spdk_nvme_ns_cmd_read 발행 → free_io_qpair 까지의 전 경로 + 다중 프로세스 안전성 + in-completion 자기 free 패턴 + reconnect 4분기 정책이 모두 주석만으로 추적 가능.

**잔여 함수** (Part 3 다음 세션): spdk_nvme_ctrlr_get_default_ctrlr_opts(80+ 줄, opts 모든 필드 기본값 설정), Intel support log pages + ANA log + supported_features + host_feature + arbitration, fail/shutdown_async/poll/get_csts_done, reset/reset_subsystem/reconnect_async/poll/disable, set_num_queues/keep_alive/host_id/configure_aer, AER 처리 5종(async_event_cb/process/queue/complete_queued/configure), multi-process 9종(get_process/get_current_process/add/remove/cleanup/free_processes/remove_inactive/proc_get_ref/put_ref/get_ref_count/proc_get_devhandle), process_init sub-callbacks 9종(vs_done/cap_done/check_en/set_en_0/set_en_0_read_cc/wait_for_ready_0/1/enable_wait_for_ready_1), public APIs 50+개.

다음 세션 Part 3 권장 순서: features/supported_log_pages → fail/shutdown → reset/reconnect → AER → multi-process → process_init sub-callbacks → public APIs.

---

**2026-04-28 (스물여덟 번째 파트 — lib/nvme/nvme_ctrlr.c Part 1 ◐)**: SPDK NVMe 드라이버에서 가장 큰 단일 파일(5997줄, 110+ 함수). 다세션 분할 작업의 첫 세션 — 핵심 8종 함수 보강.

원본 5997줄 → 주석 후 6265줄. 보강한 핵심 함수:

- **★ nvme_ctrlr_state_string**: 40+ 상태의 사람용 문자열 매핑. 상단 doc에서 6개 그룹 분류 (INIT/DISABLE/ENABLE/IDENTIFY/NS DISCOVERY/FEATURES/최종) + "WAIT_FOR_*" 패턴 의미 (비동기 명령 응답 대기). 사용자가 디버그 로그에서 controller bring-up이 어느 단계에 있는지 즉시 파악 가능.

- **_nvme_ctrlr_set_state + nvme_ctrlr_set_state/quiet 트리오**: 상태 전이 + timeout 설정의 핵심 헬퍼. NVME_TIMEOUT_KEEP_EXISTING(WAIT_FOR_* 진입 시 기존 timeout 유지), NVME_TIMEOUT_INFINITE(무한 대기), 일반 ms 값(절대 시각으로 변환). ms × ticks_per_ms 곱셈 + now_ticks 덧셈 두 단계 overflow 방어로 안전한 timeout 계산. quiet=true는 같은 상태 반복 진입 시 로그 폭주 방지 (WAIT_FOR_READY 등 폴링 단계).

- **★★★ nvme_ctrlr_process_init**: ★ controller bring-up 상태머신의 driver. 사용자 reactor 루프에서 spdk_nvme_probe_poll_async 호출할 때마다 (정확히는 nvme_ctrlr_poll_internal 경유) 호출되어 한 단계씩 진행. 호출 컨텍스트, 동작 패턴 3 stage(sleep_timeout 검사 → switch dispatch 동기/비동기 단계 → WAIT_FOR_* timeout 검사), **정상 경로 시퀀스 다이어그램** 30+ 상태 전이 문서화: INIT_DELAY → CONNECT_ADMINQ → READ_VS → READ_CAP → CHECK_EN → DISABLE/ENABLE → RESET_ADMIN → IDENTIFY → CONFIGURE_AER → SET_KEEP_ALIVE → IDENTIFY_IOCS_SPECIFIC → GET_ZNS_LOG → SET_NUM_QUEUES → IDENTIFY_ACTIVE_NS → IDENTIFY_NS 반복 → SET_SUPPORTED_LOG_PAGES → SET_SUPPORTED_FEATURES → SET_HOST_FEATURE → SET_DB_BUF_CFG → SET_HOST_ID → TRANSPORT_READY → READY.

- **nvme_robust_mutex_init_recursive_shared**: RECURSIVE + ROBUST + PSHARED 3종 속성으로 multi-process crash-safe + 같은 thread 재귀 lock 가능 mutex 초기화. nvme.c의 init_shared와 비교: driver 전역 lock은 단순(재귀 불필요), controller lock은 재귀(admin command 발행 시 nested lock).

- **★ nvme_ctrlr_construct**: 컨트롤러 객체 7단계 초기화 — INIT_DELAY vs INIT 분기(PCIe는 quirky device 대응을 위한 delay, Fabrics는 즉시 시작), admin_queue_size 3중 검증/정규화(max=4096 클램프 + quirk multiple round-up + min=2 클램프), 플래그/카운터 0 클리어, 빈 컨테이너 7종 초기화(active_io_qpairs/queued_aborts/active_procs/register_operations/ns RB tree), ★ ctrlr_lock = recursive+shared+robust mutex 초기화.

- **★ nvme_ctrlr_destruct_async**: 비동기 destruct 6단계 — is_destructed=true 마킹(★ ctrlr_probe가 이 플래그 검사 후 EBUSY로 새 attach 거부), 마지막 admin completion 흡수, queued aborts + AER abort, active IO qpair 강제 정리, shadow doorbell + IOCS-specific 데이터 free, shutdown_async 시작 (CC.SHN=01 + CSTS.SHST=10 폴링).

**남은 작업** (다음 세션, 110+ 함수 중 ~100여 개 잔여):
- opts 헬퍼 (큰 함수): get_default_ctrlr_opts, get_default_io_qpair_opts, opts_copy
- qpair 관리: alloc/free/connect/disconnect/reconnect_io_qpair
- Features 단계: set_intel_log_pages, ANA log, supported_features, host_feature
- 실패/리셋: fail, shutdown_async/poll, reset, reset_subsystem, disable
- Configure/IDs: set_num_queues, keep_alive_timeout, host_id, configure_aer
- AER: async_event_cb, process_async_event, complete_queued_async_events
- Multi-process: get_process, add/remove/cleanup, proc_get/put_ref
- process_init sub-callbacks (9종): vs_done/cap_done/check_en/set_en_0/wait_for_ready_0/1/enable_wait_for_ready_1
- Public APIs (50+): get_data/regs_*, alloc_qid/free_qid, attach_ns/detach_ns/format/update_firmware, reserve_cmb/map_pmr, boot_partition_*, security_send/recv, authenticate

다음 세션은 nvme_ctrlr.c Part 2로 계속 (qpair 관리 + features + reset 시퀀스 우선).

---

**2026-04-28 (스물일곱 번째 파트 — lib/nvme/nvme.c 완전 완료 ☑)**: NVMe 드라이버의 *프로세스 단위* 라이프사이클 진입 모듈 완결. spdk_nvme_probe() 한 줄에서 시작해 모든 controller가 READY 도달하기까지의 전체 진입 경로를 담당하는 파일.

원본 2277줄 → 주석 후 2914줄. 기존에 일부 주석이 있던 위에 **빠진 함수 30+개를 모두 보강**:

- **★ Multi-process hugepage 공유 driver 객체**: g_spdk_nvme_driver는 SPDK_NVME_DRIVER_NAME 이름의 hugepage SHARED 영역에 거주. primary가 spdk_memzone_reserve(NO_IOVA_CONTIG)로 만들고 secondary는 spdk_memzone_lookup으로 같은 가상 주소로 attach. nvme_driver_init이 g_init_mutex(process-private)로 진입 직렬화 + primary 단일 thread만 robust mutex 초기화 + secondary는 initialized=true까지 180s polling.

- **★ robust mutex via PI futex**: nvme_robust_mutex_init_shared가 PROCESS_SHARED + PTHREAD_MUTEX_ROBUST로 mutex 생성 → holder process 사망 시 다음 lock 시도자가 EOWNERDEAD 받고 consistent 호출로 복구 가능 (일반 mutex는 영구 deadlock). FreeBSD는 robust 미지원 → 일반 mutex로 대체.

- **★★ probe/attach 9단계 진입 경로**:
  1. spdk_nvme_probe → spdk_nvme_probe_ext → spdk_nvme_probe_async_ext
  2. nvme_driver_init (멱등)
  3. probe_ctx_init (init/failed 리스트 빈 상태)
  4. nvme_probe_internal: trstring 자동 채움 → transport_available_by_name 검증 → driver_lock → nvme_transport_ctrlr_scan
  5. 트랜스포트 scan에서 발견된 각 device → nvme_ctrlr_probe → probe_cb → 기존이면 ref 증가, 신규면 transport_ctrlr_construct → init_ctrlrs 추가
  6. **secondary + PCIe**: shared_attached_ctrlrs 순회 + opts/process 검증 → 자동 attach (lock unlock-during-cb 패턴)
  7. nvme_init_controllers → spdk_nvme_probe_poll_async loop
  8. nvme_ctrlr_poll_internal: process_init 한 단계 → 실패 destruct_async 누적 / READY 도달 → attached 이동 + ref + attach_cb
  9. 모든 ctrlr 처리 완료 → driver->initialized=true 마킹 + probe_ctx free

- **★ ref count 기반 detach (다중 ctrlr 컨테이너 패턴)**: spdk_nvme_detach_async가 *_detach_ctx 컨테이너에 destruct context 누적 → spdk_nvme_detach_poll_async가 SAFE 순회로 일괄 폴링 + 미완료는 INSERT_HEAD로 다시 머리에 넣어 같은 sweep에서 재검사 안 함 (FIFO 순서 보존). nvme_ctrlr_detach_async는 ref==1 (last-ref)일 때만 ctx 할당 + destruct 시작, ref>1이면 ref만 감소.

- **★ admin command 동기 polling 패턴**: nvme_completion_poll_cb가 ctlr/qpair process_completions에서 호출되면 (1) timed_out 경로 → status + dma_data 자력 free + early return (caller 떠난 상태) (2) 정상 → cpl 복사 + done=true 마킹. 짝꿍 nvme_wait_for_completion_poll이 admin lock + poll_group vs qpair 분기 + ★ PCIe CSTS register all-ones 검사 (hot-removal 빠른 감지) + timeout 검사 + done 폴링 + 에러 시 timed_out=true 마킹으로 늦은 callback이 자기 자신 free하도록 함.

- **★ user_copy 패턴**: nvme_allocate_request_user_copy가 사용자 일반 메모리 + DMA-capable 4KiB-aligned 사본 한 쌍 생성 → 완료 hook nvme_user_copy_cmd_complete가 (CONTROLLER_TO_HOST 또는 BIDIRECTIONAL이면) DMA → user 복사 + ★ PID 검증(multi-process 안전성) + dma_buffer + req cleanup + 사용자 cb 호출. 사용자가 hugepage 모름 + 일반 malloc만 알아도 admin 명령 발행 가능.

- **★ ABI 호환 SET_FIELD 매크로**: nvme_ctrlr_opts_init의 FIELD_OK(offset+sizeof <= opts_size) + SET_FIELD/SET_FIELD_ARRAY — 사용자가 구버전 헤더로 빌드해도 새 필드는 라이브러리 기본값 유지하여 안전.

- **transport_id 파싱/포맷 8종 + parse_next_key 헬퍼**: trtype/adrfam parse+str (대소문자 무관 입력, 표준 표기 출력), trid_populate_transport(매핑 테이블 5종), populate_trstring(toupper 정규화 + GCC-11 LTO false positive 회피용 수동 루프), parse_next_key(':' vs '=' 우선순위 — 둘 다 있으면 먼저 등장하는 것), transport_id_parse(인식/무시 키 분류 + 알 수 없는 키는 로그만), host_id_parse(같은 문자열을 trid와 hostid가 두 번 파싱), transport_id_compare(★ trtype 우선 + PCIe BDF 정규화로 "0000:01:00.0" vs "01:00.0" 흡수 + Fabrics 4필드 순차 + subnqn case-sensitive), prchk_flags_parse/str(reftag/guard 4 조합).

- **유틸**: nvme_request_check_timeout(admin/AER/KEEP_ALIVE 분기 + ★ multi-process PID 검사 — 다른 프로세스 발행 req 책임 회피), nvme_parse_addr(getaddrinfo 래퍼 + gai 코드 음수 errno 정규화), nvme_get_default_hostnqn(UUID NQN 표준 형식 "nqn.2014-08.org.nvmexpress:uuid:...").

**결과 — NVMe 드라이버 진입 메커니즘 완결**: 사용자가 spdk_nvme_probe() 호출하는 순간부터 모든 controller가 READY 도달하기까지의 전 경로, multi-process hugepage 공유 driver 객체 라이프사이클, robust mutex의 PI futex crash-safe 메커니즘, admin completion 동기 polling 패턴(timed_out 자동 free 포함), user_copy 헬퍼의 DMA 사본 패턴, ABI 호환 SET_FIELD 매크로, transport_id 파싱/비교의 PCIe vs Fabrics 분기가 모두 주석만으로 추적 가능.

다음 세션 후보:
- `lib/nvme/nvme_ctrlr.c` (5997줄) — ★★★ 컨트롤러 상태머신. 매우 큼, 2-3 세션 분할 필요. process_init의 40+ 상태 전이가 핵심
- `lib/nvme/nvme_ns.c` — namespace identify + 설정
- `lib/nvme/nvme_ctrlr_cmd.c` — admin 커맨드 빌더 (Identify, Set Features, Get Log Page)

---

**2026-04-28 (스물여섯 번째 파트 — lib/nvme/nvme_auth.c 완전 완료 ☑)**: NVMe-oF 호스트 측 DH-HMAC-CHAP 인증 프로토콜 완결. nvme_fabric.c의 CONNECT 응답에서 atr/ascr 플래그가 셋되면 후속 인증을 8상태 비동기 머신으로 처리하는 파일. NVMe-oF 1.1 스펙 Section 8.13 완전 구현.

원본 1296줄 → 주석 후 2397줄. 핵심 완료:

- **★ DH-HMAC-CHAP 5대 가치 + 와이어 메시지 6종 시퀀스 문서화**: PSK 비공개 (키 자체는 와이어 미전송, HMAC challenge-response), DH 키 교환 옵션 (NULL/ffdhe2048~8192 RFC 7919, forward secrecy), 상호 인증 옵션 (dhchap_ctrlr_key 시 컨트롤러 신원도 검증), 해시 강도 협상 (SHA-256/384/512), 8상태 비동기 폴링.

- **★ DHHC-1 키 포맷 파서**: "DHHC-1:HH:base64-encoded-key-with-crc32:" 형식 — sscanf로 헤더 hash 16진수 파싱 → strstr로 trailing ':' 찾아 NUL 종결 → spdk_base64_decode → 36/52/68B 사이즈 검증 (각 SHA-256/384/512 키 32/48/64B + 4B CRC) → spdk_crc32_ieee_update IEEE 다항식으로 끝 4B 검증 → spdk_memset_s 보안 메모리 클리어.

- **★★ NVMe-oF 8.13.5 키 변환 알고리즘**: nvme_auth_transform_key(key, hash, nqn, ...) = HMAC(key, nqn || "NVMe-over-Fabrics") — NQN과 결합 + 도메인 분리 문자열로 같은 PSK를 다른 NQN/프로토콜에서 쓸 때 다른 키 도출. NONE 모드는 raw 복사.

- **★★ DH secret으로 challenge nonce 보강** (augment_challenge): caval = HMAC(MD(key=dhsec), cval) — DH secret을 한 번 다이제스트해서 HMAC 키로, 입력은 원본 challenge nonce. 결과: 같은 cval이라도 매 세션 dhsec에 따라 다른 caval → forward secrecy.

- **★★ HMAC 응답 계산 8 입력 누적** (spdk_nvme_dhchap_calculate, 공개 API): caval || seq || tid || scc || type || nqn1 || NUL || nqn2 모두 HMAC update. type 인자 "HostHost" vs "Controller"로 양방향 인증의 도메인 분리, nqn1/nqn2 순서가 방향에 따라 hostnqn↔subnqn 뒤바뀜.

- **DH 키 관리 + ★ reserved_req 사용**: DHX 알고리즘 (ffdhe* 그룹), OSSL_PARAM_BLD로 peer pubkey import, EVP_PKEY_dup으로 ctx 종속성 끊기, set_dh_pad(1)로 secret 길이 일관성. AUTH 메시지 제출 시 **qpair->reserved_req 격리 슬롯** 사용 — 일반 free_req 풀이 비어 있어도 인증은 항상 가능 (qpair init 시 보존).

- **★★★ nvme_auth_send_reply 7단계 코어**: auth->hash 보존 → DH 키페어 생성 + 호스트 pubkey 추출 + DH secret 도출 → ctrlr->opts에서 PSK + 컨트롤러 키 dup → "HostHost" type으로 호스트 응답 rval 계산 → 상호인증 시 (a) seqnum 채번 (b) RAND_bytes로 ctrlr_challenge nonce 생성 (c) "Controller" type으로 컨트롤러 기대 응답 미리 계산해 auth->challenge에 보존 → in-place dma_data 재사용해 reply 빌드 (rval[0..hl] 호스트 응답 + rval[hl..2hl] ctrlr_challenge — cvalid=0이여도 슬롯 강제 + rval[2hl..2hl+publen] host pubkey + 헤더 cvalid/dhvlen/seqnum).

- **상호 인증 검증 정점** (check_success1): ctrlr_key 있으면 rvalid=1 강제 + hl 일치 + ★ memcmp(msg->rval, auth->challenge) — send_reply에서 미리 계산해둔 기대값과 컨트롤러가 실제로 보낸 응답을 바이트 단위 비교. 같으면 컨트롤러가 진짜 같은 PSK를 갖고 있다는 증명.

- **★★★★ 8상태 머신 driver** (authenticate_poll): NEGOTIATE→AWAIT_NEGOTIATE→AWAIT_CHALLENGE→AWAIT_REPLY→AWAIT_SUCCESS1→{AWAIT_SUCCESS2 if ctrlr_key else DONE}/AWAIT_FAILURE2→DONE. **핵심 패턴**:
  - **in_auth_poll 재진입 가드**: cb_fn 안에서 process_completions 재호출 방어
  - **do-while + prev_state 비교**: 상태 전이가 있으면 즉시 다음 단계 진행, 같은 상태면 -EAGAIN
  - **NEGOTIATE 의도적 early return**: initial kick에서 응답 없이 다음 단계 진입 방지
  - **각 AWAIT_*의 3분기**: nvme_wait_for_completion_poll의 -EAGAIN(대기) / error(완료 자체 에러) / 0(성공 → 다음 단계)
  - **AWAIT_SUCCESS1 분기**: ctrlr_key 있으면 send_success2 → AWAIT_SUCCESS2, 없으면 즉시 DONE
  - **AWAIT_SUCCESS2/FAILURE2 통합 처리**: 둘 다 마지막 송신 응답만 받으면 DONE
  - **DONE**: nvme_fabric_qpair_poll_cleanup + nvme_fabric_qpair_auth_cleanup(cb_fn 호출) + auth->status 반환

- **kick-start + EALREADY 동시 진행 방지**: nvme_fabric_qpair_authenticate_async가 dhchap_key/ascr 검증 + 4KiB DMA zmalloc + ctrlr 단위 tid 채번 + 첫 polling으로 NEGOTIATE 송신 트리거(-EAGAIN→0 정규화). spdk_nvme_qpair_authenticate(사용자 노출 API)는 cb_fn != NULL 검사로 동시 진행 차단 후 트랜스포트 위임.

**결과 — NVMe-oF 호스트 인증 전 메커니즘 완결**: PSK keyring → DHHC-1 형식 파싱 + CRC 검증 → 키 변환(HMAC + NQN 결합) → 8상태 머신 → DH 키 교환(forward secrecy) → 8 입력 HMAC 응답 계산 → 양방향 인증 → cleanup + cb_fn 호출의 모든 단계 + 실패 분기 + 재진입 방어 + 보안 메모리 클리어 패턴이 주석만으로 완전 추적 가능. nvme_fabric.c의 atr/ascr 플래그 후속 흐름 완성.

다음 세션 후보:
- `lib/nvme/nvme.c` (2277줄) — nvme_internal.h의 nvme_complete_request 등 공용 헬퍼 구현
- `lib/nvme/nvme_ctrlr.c` (5997줄) — 컨트롤러 상태머신 (매우 큼, 다세션 분할 필요)
- `lib/nvme/nvme_ns.c` — namespace identify 및 설정

---

**2026-04-28 (스물다섯 번째 파트 — lib/nvme/nvme_poll_group.c 완전 완료 ☑)**: NVMe 다중 트랜스포트 폴링 그룹의 상위 추상 완성. 이전까지 nvme_transport.c가 트랜스포트별 sub-group(tgroup)만 노출했다면, 이 파일은 그것들을 **단일 spdk_thread/reactor에서 단일 process_completions로 묶는 최상위 컨테이너** + **interrupt 모드 epoll 통합 진입점**.

원본 515줄 → 주석 후 1248줄. 핵심 완료:

- **★ accel_fn_table ABI 호환 패턴**: `SET_FIELD` 매크로(offset+sizeof <= table_size 검사로 구버전 사용자 헤더에서도 안전 복사) + `SPDK_STATIC_ASSERT(sizeof == 56)`(필드 추가 시 크기 갱신 강제) + 콜백 일관성 2단계 검증(finish/reverse/abort XOR 표현식, append→finish 의존성).

- **★ Linux eventfd 트리오로 disconnect 비동기 통지 메커니즘 구현**: 트랜스포트가 link loss/reset 감지 시 `nvme_poll_group_write_disconnect_qpair_fd`에서 `eventfd(EFD_NONBLOCK|EFD_CLOEXEC)`에 8B write → epoll fd가 readable → `spdk_fd_group_wait`가 깨어남 → 자동으로 등록된 `nvme_poll_group_read_disconnect_qpair_fd` 콜백 → 사용자 `interrupt.cb_fn` 호출 → 사용자가 `process_completions`를 즉시 재호출하여 `disconnected_qpair_cb`로 처리. 비-Linux 환경은 stub 제공.

- **★★ spdk_nvme_poll_group_add 5단계**: (1) NVME_QPAIR_DISCONNECTED 검증 — connect 전에만 등록 가능, (2) **enable_interrupts_is_valid first-time 결정 정책** — 첫 qpair의 컨트롤러 옵션으로 그룹 모드 고정 + 이후 qpair는 일치 강제(혼용 금지로 폴링 루프 의미 유지), (3) STAILQ에서 같은 트랜스포트의 tgroup 검색, (4) **dlopen 시나리오 지원** — 못 찾으면 nvme_get_first/next_transport로 트랜스포트 레지스트리 순회 + lazy create, (5) nvme_transport_poll_group_add 위임.

- **★ connect/disconnect 순서 정합성**: `nvme_poll_group_connect_qpair`는 트랜스포트 connect → fd_group 등록. **fd 등록 실패 시 트랜스포트 disconnect로 롤백**(고아 connected qpair 방지). 반대로 `disconnect_qpair`는 fd_group 제거 → 트랜스포트 disconnect 순서로, **disconnect 도중 epoll wake로 죽은 qpair 폴링 시도 방지**.

- **★★★ spdk_nvme_poll_group_process_completions의 hot-path 코어**: poll_group의 메인 폴링 진입점. **`in_process_completions` 재귀 가드**(사용자 cb_fn 안에서 재호출 시 0 반환 — 중복 처리 방지), 모든 tgroup STAILQ 순회로 트랜스포트별 위임, **error_reason 첫 음수 보존 + num_completions 양수 누적**(에러는 1번만 격상하고 양수는 누적 — 사용자가 부하 측정 + 진단 신호 동시 활용), `spdk_unlikely`로 hot-path 분기 예측 최적화.

- **★ destroy 롤백 패턴**: `STAILQ_FOREACH_SAFE` + `STAILQ_REMOVE`로 안전 순회, 트랜스포트별 destroy가 -EBUSY(qpair 잔존) 반환 시 `STAILQ_INSERT_TAIL`로 다시 끝에 넣어 일관성 회복하고 EBUSY 전파(사용자 재시도 가능). fd_group 해제 시 disconnect_qpair_fd remove → close → fd_group destroy 정확한 3단계 순서.

- **★ get_stats 2단계 패턴 + 부분 실패 허용**: 1차 STAILQ 순회로 transports_count 측정 + 2차 순회로 트랜스포트별 stats 수집(반환 0인 것만 reported_stats_count로 카운트). 모든 트랜스포트가 stats 미구현이면 -ENOTSUP. free_stats는 trtype 매칭으로 트랜스포트별 free_stats 위임 + `freed_stats == num_transports` assert로 누락 검증.

**결과 — poll_group의 폴링/통지/통계 전 메커니즘 추적 가능**: 사용자 reactor 루프가 매 tick `spdk_nvme_poll_group_process_completions(group, 0, my_disconnected_cb)`를 호출했을 때, group → tgroups[PCIe, RDMA, TCP, ...] STAILQ 순회 → 트랜스포트 vtable → qpair 완료 수확 → cb_fn → free_tr 반환 + disconnected qpair 4가지 통지 경로(STAILQ 즉시 / 정기 폴링 / eventfd epoll / 사용자 callback) + interrupt 모드 fd_group 통합(qpair fd + disconnect eventfd 단일 epoll)이 모두 주석만으로 완전 추적 가능.

다음 세션 후보:
- `lib/nvme/nvme_auth.c` (1296줄) — DH-CHAP 인증 상태머신 (nvme_fabric.c의 atr/ascr 플래그 후속 흐름)
- `lib/nvme/nvme.c` (2277줄) — nvme_complete_request 등 nvme_internal.h의 공용 헬퍼 구현
- `lib/nvme/nvme_ctrlr.c` (5997줄) — 컨트롤러 상태머신 process_init / enable / disable (매우 큼, 다세션 분할 작업 필요)

---

**2026-04-26 (스물네 번째 파트 — lib/nvme/nvme_fabric.c 완전 완료 ☑)**: NVMe-oF 트랜스포트 공통 레이어. PCIe NVMe와 NVMe-oF의 본질적 차이를 드러내는 파일 — PCIe는 BAR mmap으로 레지스터를 직접 MMIO로 R/W하지만, NVMe-oF는 **Fabric Property Set/Get 커맨드(opcode 0x7F + fctype 0x00/0x04)**로 메시지화. 이 파일이 그 변환 계층 + Discovery 서비스 + CONNECT 핸드셰이크를 모두 담당.

원본 671줄 → 주석 후 1578줄. 핵심 완료:

- **★ Property R/W**: prop_set_cmd/sync/done/async + prop_get 동일 패턴 + 공개 reg_4/8 set/get sync/async 8종. 트램펄린 콜백(spdk_nvme_cmd_cb → spdk_nvme_reg_cb 시그니처 변환), nvme_fabric_prop_ctx의 value/size 보존 차이, robust=true/false 시 status leak 정책.
- **★ Discovery 흐름**: discover_probe(NQN/traddr/trsvcid 정규화 + nvme_ctrlr_probe 재귀), get_discovery_log_page(LID=0x70), ctrlr_discover(4KB 버퍼 페이징 — 첫 페이지 16B 헤더 + 3 entries vs 이후 4 entries, recfmt=0 검증, numrec/offset 누적), ctrlr_scan(직접 vs DISCOVERY_NQN 분기 + direct_connect 모드).
- **★★ qpair_connect_async**: NVMe-oF CONNECT 9단계 — 인자 검증 → DMA 1024B nvmf_data → status calloc → SQE 빌드(opcode=0x7F, fctype=0x01, qid, sqsize=N-1, kato) → **reserved_req 사용**(free_req 풀 부족 대비 격리 슬롯) → 페이로드(admin=cntlid 0xFFFF / I/O=ctrlr->cntlid + hostid 16B + hostnqn + subnqn) → submit → timeout_tsc + fabric_poll_status 보존.
- **★ qpair_connect_poll**: 응답 폴링 + cntlid 추출(controller association 핵심 키) + atr/ascr 인증 플래그 추출. status_code_specific union의 success/invalid 분기, sct/sc 진단 로깅.
- **★ auth_required**: atr || ascr || dhchap_ctrlr_key || auth.cb_fn 4-OR 정책 (컨트롤러 요구·호스트 정책·사용자 명시 통합).
- **수명주기**: poll_cleanup(timed_out=true 시 leak 허용 → late callback use-after-free 회피), auth_cleanup(idempotent).

**결과 — NVMe-oF 진입점 완비**: PCIe vs NVMe-oF의 레지스터 R/W 추상화 차이, Discovery 서비스의 다단계 페이징, CONNECT 와이어 포맷 1024B와 admin=0xFFFF controller 할당, 인증 진입 결정 정책 4가지가 모두 주석만으로 추적 가능.

다음 세션 후보:
- `lib/nvme/nvme_auth.c` — DH-CHAP 인증 상태머신 (이번 atr/ascr 후속 흐름)
- `lib/nvme/nvme_poll_group.c` — poll group 상위 추상 (transport tgroup과 쌍)
- `lib/nvme/nvme_tcp.c` 또는 `nvme_rdma.c` — 실제 트랜스포트 구현 (이 파일의 사용자)
- `lib/nvme/nvme_ctrlr.c` — 컨트롤러 상태머신 (process_init이 fabric의 set_reg_4_async 호출)

---

**2026-04-25 (스물세 번째 파트 — lib/nvme/nvme_pcie.c 완전 완료 ☑)**: PCIe 트랜스포트의 cold-path 구현 파일 완결. 이로써 **SPDK NVMe 드라이버의 다섯 기둥이 모두 ☑ 완결** — 애플리케이션 `spdk_nvme_probe()`부터 장치 DMA 완료 콜백까지의 전 여정이 주석만으로 추적 가능.

핵심 완료:
- **파일 상단 4섹션 블록**: PCIe cold-path 5대 책임(probe/BAR/MMIO/SIGBUS/CMB-PMR) + probe→attach 호출 체인 + `SPDK_NVME_TRANSPORT_REGISTER` 등록 원리. nvme_pcie_common.c(hot-path)와의 역할 분리 명시.

- **★ SIGBUS 방어 메커니즘** (`nvme_sigbus_fault_sighandler`): PCIe link loss 중 MMIO 접근 → SIGBUS 발생 → 일반 핸들러면 crash. SPDK는 이 핸들러로 **BAR 영역을 anonymous 메모리로 MAP_FIXED remap + 0xFF로 채움** → 후속 MMIO read는 all-ones를 반환하지만 프로세스 생존. `g_thread_mmio_ctrlr` TLS 마커로 어느 컨트롤러가 SIGBUS 냈는지 식별. atomic CAS(`g_signal_lock`)로 재진입 방지.

- **★ MMIO 레지스터 R/W** (set/get_reg_4/8): `spdk_mmio_*` 경유 volatile 접근. all-ones 감지로 link 무효 추정. ASQ/ACQ/AQA/CMBLOC/CMBSZ/PMRCAP/PMRCTL/PMRSTS/PMRMSCL/PMRMSCU 각 레지스터별 wrapper.

- **★ CMB 5종** (map_cmb/unmap_cmb/reserve_cmb/map_io_cmb/unmap_io_cmb): CAP.CMBS 확인 → CMBSZ의 SZU(unit size = 2^(12+4×SZU))/SZ 해석 + CMBLOC의 BIR/OFST 계산 → BAR mmap → 경계 검증. 공개 map_io_cmb는 WDS/RDS 최소 하나 + 4MiB 이상 + 2MB 정렬 후 `spdk_mem_register`로 DPDK 힙에 등록.

- **★ PMR 6종** (map/unmap/config/enable/disable/map_io/unmap_io): CMSS 지원 시 PMRMSCU(상위 32b) + PMRMSCL(하위 32b + CMSE=1) 순서로 CBA 설정 → PMRSTS.CBAI 확인. `config_pmr`의 PMRCTL.EN 토글 + PMRSTS.NRDY 대기 루프 + PMRCAP.PMRTO/PMRTU 기반 timeout (0=500ms 단위, 1=1분 단위).

- **★★ allocate_bars**: BAR0 mmap → `pctrlr->regs` 설정 → `doorbell_base = &regs->doorbell[0].sq_tdbl` 계산(★ 이 포인터가 nvme_pcie_common.c의 qpair_construct가 각 SQ/CQ doorbell 위치 산출의 기반) → CMB/PMR 매핑 시도.

- **★★ ctrlr_construct**: 10단계 시퀀스 — pci_claim → 구조체 할당(SHARE hugepage) → opts/quirks/NUMA → 공통 construct → allocate_bars → PCI CMD 0x404(Bus Master Enable + INTx Disable) → CAP read + doorbell_stride(2^DSTRD dword) → admin qpair 생성 → 프로세스 등록 → 최초 시 SIGBUS 핸들러 등록.

- **★★ ctrlr_scan/pcie_nvme_enum_cb**: DPDK `spdk_pci_enumerate` 경유 — primary는 traddr 필터 + `nvme_ctrlr_probe`, secondary는 primary 구축 컨트롤러에 `nvme_ctrlr_add_process`로 자기 컨텍스트만 추가. interrupt 모드는 secondary 금지.

- **★ ctrlr_enable**: ASQ/ACQ 물리 주소 + AQA(크기 0-based) 설정 — CC.EN=1 이전 preparation (CC.EN 자체는 공통 nvme_ctrlr.c가 처리).

- **★★★ pcie_ops vtable**: 42개 함수 포인터로 `spdk_nvme_transport_ops` 전체 채움. 수명주기 6 + 레지스터 5 + limits 2 + CMB 3 + PMR 4 + qpair 수명주기 4 + qpair hot-path 7 + poll group 10 + name/type 2. 이 파일(cold-path)과 nvme_pcie_common.c(hot-path) 양쪽에 정의된 함수를 모두 연결.

- **★★★ SPDK_NVME_TRANSPORT_REGISTER(pcie, &pcie_ops)**: 파일 맨 아래의 이 한 줄이 존재 이유. `__attribute__((constructor))`로 전개되어 main() 전에 자동 실행 → `spdk_nvme_transport_register(&pcie_ops)` → nvme_transport.c의 `g_spdk_nvme_transports` TAILQ에 PCIe ops 삽입 → 이후 상위 레이어가 `nvme_get_transport("PCIE")`로 조회 가능 → vtable dispatch가 이 파일로 연결.

**결과 — SPDK NVMe 드라이버의 다섯 기둥 모두 완결**:
  (1) ☑ `lib/nvme/nvme_ns_cmd.c`     — 사용자 API → nvme_request 번역
  (2) ☑ `lib/nvme/nvme_qpair.c`      — 제출/완료/상태머신 코어
  (3) ☑ `lib/nvme/nvme_transport.c`  — vtable 디스패치 (multi-process)
  (4) ☑ `lib/nvme/nvme_pcie_common.c` — doorbell/PRP/MMIO hot-path
  (5) ☑ `lib/nvme/nvme_pcie.c`       — PCIe attach/BAR/SIGBUS (이번 세션)

이제 **애플리케이션이 `spdk_nvme_probe()` 호출하는 순간부터, DPDK enumerate → pcie_nvme_enum_cb → ctrlr_construct → BAR mmap → admin 큐 준비 → CC.EN=1 → identify → I/O qpair 생성 → `spdk_nvme_ns_cmd_read()` → nvme_request 빌드 → SQ write → doorbell MMIO → 장치 fetch → DMA → CQE 기록 → phase bit polling → cb_fn 실행 → free_tr 반환까지 전 여정**이 주석만으로 완결 추적 가능.

다음 세션은 `lib/nvme/nvme_fabric.c` (NVMe-oF CONNECT/AUTH 공통 로직) 또는 `lib/nvme/nvme_poll_group.c` (transport poll group 상위 추상) 중 선택.

---

**2026-04-24 (스물두 번째 파트 — lib/nvme/nvme_pcie_common.c 잔여 inline 마무리 ☑)**: 이전 세션에서 부분완료(◐)로 남긴 파일을 완결(☑)로 전환. 3개 빌더 함수 본문의 라인별 inline 주석 완료로 파일 전체 주석 충족.

완료한 세 함수의 내부 본문:
- **`build_contig_hw_sgl_request`**: CONTIG 페이로드를 vtophys 호출로 물리 segment들로 분해하여 SGL descriptor 배열 생성. `mapping_length` IN/OUT 변수로 hugepage 경계 고려, 단일 descriptor inline 최적화 vs 다중 LAST_SEGMENT 분기 명시. ubsan NULL 포인터 경고 회피용 이중 cast 설명.
- **`build_hw_sgl_request`**: SGL→SGL 변환의 핵심. next_sge_fn 반복 호출, Bit Bucket SGL 특수 처리(UINT64_MAX sentinel, READ 전용, NLB 포함 규약), 내부 루프에서 사용자 SGE를 물리 segment들로 쪼개기, ★ SGL merge 최적화(이전 descriptor와 물리 연속이면 length 확장).
- **`build_prps_sgl_request`**: SGL→PRP 변환. prp_list_append 누적 호출로 여러 SGE를 하나의 PRP 리스트로 통합, 중간 SGE의 페이지 경계 정렬 assert로 방어 검증 (상위 `_nvme_ns_cmd_split_request_prp`가 이미 보장했어야 하는 스펙 제약).

**결과**: 원본 1912줄 → 주석 후 3313줄. 이로써 SPDK NVMe 드라이버 이해의 다섯 기둥 중 **nvme_pcie_common.c가 완전 완결**.

다섯 기둥 완성 상태:
  (1) ☑ nvme_ns_cmd.c     — 사용자 API → nvme_request 번역
  (2) ☑ nvme_qpair.c      — 제출/완료/상태머신 코어
  (3) ☑ nvme_transport.c  — vtable 디스패치
  (4) ☑ nvme_pcie_common.c — 실제 doorbell/PRP/MMIO (이번 세션 마무리)
  (5) [다음] nvme_pcie.c  — PCIe attach/BAR mapping 구체화 (DPDK enumerate, MSI-X)

다음 세션은 `lib/nvme/nvme_pcie.c` (1173줄) — PCIe 트랜스포트의 **attach/probe/BAR 매핑**. 이 파일을 마치면 장치 발견부터 I/O hot-path까지 PCIe 경로 전체가 완결된다.

---

**2026-04-24 (스물한 번째 파트 — lib/nvme/nvme_pcie_common.c 핵심 완료 ◐)**: **I/O 경로 구현 파일의 정점 도달**. 이 파일에서 "1 IOPS가 어떻게 실제 CPU에서 처리되는가"가 드러난다 — PRP 리스트 빌드, SQ write, doorbell MMIO, phase bit polling, CQE 복원까지. 애플리케이션 `spdk_nvme_ns_cmd_read()` → 장치 fetch → CQE → cb_fn의 전 여정이 이제 주석만으로 추적 가능.

핵심 완료:
- **파일 상단 4섹션 블록**: SPDK NVMe 드라이버의 "실제 submit/complete 호출 체인" 6 레이어 그래프 + nvme_pcie_qpair/tracker 자료구조 레이아웃 + hot-path의 polled-mode 본질 + multi-process 관점 + g_thread_mmio_ctrlr TLS 마커

- **★★★★ `nvme_pcie_qpair_submit_request`** — submit 진입 5단계: ctrlr_lock(admin) → free_tr pop + outstanding 이동 + QD++ → req 연결 + req->cmd.cid = tr->cid → PSDT=PRP 기본 + sgl_supported 판정 + DSM quirk + dword alignment → build_req_fn 분기 테이블 dispatch + build_metadata → submit_tracker. 에러 처리 철학(-EFAULT 상위 전파 금지) 명시.

- **★★★★ `nvme_pcie_qpair_submit_tracker`** — 실제 device-notification 지점: TRACE_SUBMIT → fuse 추적 → SSE2 non-temporal 128b×4 SQE copy (QEMU 시 8B×8 fallback) → sq_tail wrap → doorbell MMIO write. 이 한 줄(`ring_sq_doorbell`)이 "호스트→장치" 경계.

- **★★★★ `nvme_pcie_qpair_process_completions`** — 완료 폴링의 정점: 7단계 (state check, CONNECTING admin 대리 폴링, ctrlr_lock, max_cap 적용, phase bit 루프 with next prefetch and aarch64/PPC memory barrier, tracker 복원 + complete_tracker, CQ doorbell ring + delay_cmd_submit flush + timeout + admin pending + vtophys failure 지연). ★ phase bit polling이 "CPU polled-mode NVMe"의 본질 — MMIO read 없이 RAM cache hit로 완료 감지.

- **★★★ `nvme_pcie_qpair_complete_tracker`** — 개별 CQE 처리: TRACE_COMPLETE → retry 판정(DNR + is_retry + count) → outstanding 제거 + QD-- → multi-process insert or nvme_complete_request → cb_fn 호출 → free_tr 반환.

- **★★★ `nvme_pcie_prp_list_append`** — PRP 리스트의 심장: 3-mode (PRP1 only / PRP2 direct / PRP list 주소) + 4KB 페이지 경계 엄격 정렬 (첫 페이지는 offset 허용, 이후는 page-aligned 강제) + tr->u.prp 배열 활용 + prp_sgl_bus_addr 물리 주소 계산 (construct 시 offsetof로 미리 세팅).

- **★★ `nvme_pcie_qpair_build_metadata`** — separate metadata 3-path: (a) SGL_MPTR_SGL 완전 SGL / (b) MPTR에 물리주소 직접 / (c) 물리 불연속 시 실패. PSDT 필드 승격 로직 + `cmd.mptr = prp_sgl_bus_addr - sizeof(sgl_descriptor)` 트릭 (배열 직전 16B에 meta_sgl 배치 약속).

- **★ `nvme_pcie_qpair_construct`** — SQ/CQ/tracker 메모리 레이아웃 완전 설명: CQ 크기의 3/4만 tracker (wrap 방지 여유), 64B-aligned tracker 풀 단일 할당 (배열 인덱싱 + PRP list 4KB 경계 미횡단), shadow doorbell 필드 초기화, PCIe doorbell_base stride 계산.

- **connect 비동기 체인** — Create CQ → create_cq_cb → Create SQ → create_sq_cb → READY (+ shadow doorbell setup). SQ 실패 시 CQ 되돌리기 (nvme_completion_sq_error_delete_cq_cb). defer_destruction 경로 — 사용자가 connect 중 delete 요청한 race 처리.

- **multi-process admin 완료** — 다른 프로세스의 admin 요청 CQE를 내 프로세스가 발견했을 때 insert_pending_admin_request로 그 프로세스 active_reqs에 보류 → 그 프로세스가 process_completions 때 complete_pending_admin_request로 자기 cb_fn 실행. nvme_qpair.c의 nvme_complete_register_operations와 비슷한 멀티프로세스 안전성 패턴.

- **Build req 분기 테이블** — 2D 배열 `[payload_type][sgl_supported]`로 4가지 빌더 중 O(1) 선택.

- **PCIe poll group 10종** — comp_channel 없는 순차 폴링 모델 + g_dummy_stat NULL 방지 + SPDK_TRACE_REGISTER_FN 추적점 등록.

**남은 작업 (◐)**: build_hw_sgl_request / build_prps_sgl_request 본문의 내부 루프 인라인 주석. 두 함수는 함수 헤더는 상세 주석되어 있으나 내부 본문(대략 각 80-100줄)은 인라인 세부 주석 미완. 다음 세션에서 보강 필요.

**결과 — SPDK NVMe 드라이버 이해의 다섯 기둥 완성**:
  (1) nvme_ns_cmd.c     — 사용자 API → nvme_request 번역
  (2) nvme_qpair.c      — 제출/완료/상태머신 코어
  (3) nvme_transport.c  — vtable 디스패치
  (4) nvme_pcie_common.c — 실제 doorbell/PRP/MMIO (이번 세션)
  (5) [다음] nvme_pcie.c — PCIe attach/BAR mapping 구체화

이로써 "spdk_nvme_ns_cmd_read()부터 장치 SQE fetch까지" 전 여정을 주석만 읽으면 이해할 수 있다.

---

**2026-04-24 (스무 번째 파트 — lib/nvme/nvme_transport.c 완전 완료)**: **I/O 경로 아키텍처의 세 번째 기둥 완성**. nvme_ns_cmd.c(사용자 API→nvme_request 번역) + nvme_qpair.c(제출/완료/상태머신) + **nvme_transport.c(vtable 디스패치)** 삼박자. 이제 상위 코드가 어떤 트랜스포트(PCIe/RDMA/TCP/VFIOUSER/CUSE)인지 모른 채 작동하는 메커니즘이 명확해짐.

핵심 완료:
- **파일 상단 4섹션 블록**: vtable 디스패치 패턴 원리, 두 조회 경로(qpair->transport 캐시 vs nvme_get_transport 조회) 공존 이유를 **multi-process 안전성**으로 상세 설명. PCIe 멀티프로세스에서 각 프로세스가 자기 g_transports[] 정적 배열에 서로 다른 함수 포인터를 갖기 때문에 admin 큐는 매 호출마다 trstring 기반 lookup 필요.

- **★★ async fallback 패턴의 해부**: `register_operations` 큐잉 메커니즘 — 트랜스포트가 async 레지스터 op를 구현 안 했을 때 동기 호출 + 가짜 완료 큐잉 → 다음 admin `process_completions`가 `nvme_complete_register_operations`(nvme_qpair.c에서 이미 주석된 함수)로 실행. 이렇게 함으로써 상위 코드는 "async" 단일 API로만 작성되어도 PCIe 같은 본질적으로 동기인 트랜스포트에서 동작.

- **★★★ Hot-path 5종의 공통 디스패치 패턴**: abort_reqs/reset/submit_request/process_completions/iterate_requests 모두 `spdk_likely(!is_admin)→qpair->transport 캐시` vs `else→nvme_get_transport 재조회` 분기. 특히 submit_request와 process_completions는 nvme_qpair.c의 `_nvme_qpair_submit_request`와 `spdk_nvme_qpair_process_completions`가 위임하는 최종 관문.

- **컨트롤러 수명주기**: construct→scan→enable→enable_interrupts→ready→...→destruct 시퀀스. 각 함수에 트랜스포트별 구현 예시(PCIe MMIO vs Fabrics property capsule)와 `nvme_ctrlr_process_init` 상태머신 연결.

- **CMB/PMR 고급 기능**: Controller Memory Buffer(장치 내부 메모리, 휘발성)와 Persistent Memory Region(비휘발성, NVMe 1.4) 각각의 호출 시퀀스(reserve/enable→map→unmap→disable), PCIe 전용성, BaM 같은 고성능 라이브러리 사용처까지 문서화.

- **qpair connect/disconnect 상세**: `nvme_transport_ctrlr_connect_qpair`의 동기/비동기 busy-wait 로직(CONNECTING 상태 유지 → fabrics+poll_group은 group 단위 폴링, 그 외는 qpair 단일 폴링), `disconnect_qpair_done`의 active_proc 기반 조건부 abort + interrupt-mode poll_group fd event write + fabric poll/auth cleanup.

- **Poll Group 관리**: `transport_poll_group` 내부 구조 — connected/disconnected 두 STAILQ로 qpair 분리 관리, `qpair->poll_group_tailq_head` 포인터로 소속 리스트 O(1) 판별. connect/disconnect_qpair의 리스트 이동 로직과 `num_connected_qpairs` 카운터 유지 불변식. 이것이 NVMe-oF 멀티 qpair 폴링 성능의 기반.

- **ABI 호환 opts 패턴**: `spdk_nvme_transport_get_opts/set_opts`의 `SET_FIELD` 매크로 + `opts_size` 필드 — 새 필드 추가 시 이전 SPDK 빌드 애플리케이션과도 호환성 유지. `SPDK_STATIC_ASSERT(sizeof==32)`로 크기 드리프트 방지.

**결과**: SPDK NVMe 드라이버 아키텍처 이해의 **네 기둥** 완성:
  (1) nvme_ns_cmd.c — 사용자 API → nvme_request 번역
  (2) nvme_qpair.c  — 제출/완료/상태머신 코어
  (3) nvme_transport.c — vtable 디스패치
  (4) [다음] nvme_pcie_common.c — 실제 doorbell/PRP 빌드 (트랜스포트 구현)

다음 세션은 `lib/nvme/nvme_pcie_common.c` (1912줄) 진입 — doorbell ring, PRP 빌드, completion poll의 실제 구현. 이 파일을 마치면 애플리케이션 spdk_nvme_ns_cmd_read() 호출부터 장치 SQE fetch까지의 전 여정이 주석만으로 완결.

---

**2026-04-24 (열아홉 번째 파트 — lib/nvme/nvme_qpair.c 완전 완료)**: **I/O 경로 코어 구현 파일 두 번째 완료**. nvme_ns_cmd.c가 "사용자 API → nvme_request 변환"이었다면, nvme_qpair.c는 "nvme_request → 트랜스포트 submit & 완료 콜백" — 트랜스포트 독립적 제출/완료 코어. 이제 `spdk_nvme_ns_cmd_read` → `_nvme_ns_cmd_rw` → `nvme_qpair_submit_request` → `_nvme_qpair_submit_request` → `nvme_transport_qpair_submit_request` 전 경로가 주석만으로 연결 가능.

핵심 완료:
- **파일 상단 4섹션 블록**: submit 호출 체인(6 레이어), complete 호출 체인, reactor poller 컨텍스트, 공유 자료구조 6종(free_req/queued_req/aborting_queued_req/err_req_head/err_cmd_head/reserved_req) 전체 설명
- **★★★ `spdk_nvme_qpair_process_completions`** — SPDK NVMe의 심장. 7단계 상세: [1] admin 선행 (register 완료 + transport events) [2] is_failed/is_removed 처리 [3] check_enabled [4] error injection 완료 [5] 트랜스포트 CQ 드레인 (실제 CQE 수확 + cb_fn 실행) [6] delete_after_completion 지연 처리 [7] resubmit (완료 개수만큼 flow control)
- **★★★ `_nvme_qpair_submit_request`** — 9단계 제출 코어. split parent의 children 재귀 제출, error injection 테이블 매칭, submit_tick 기록, ENABLED/FABRIC-CONNECTING 허용 조건, trasnport submit delegation, EAGAIN 재큐잉 처리
- **★ `nvme_qpair_check_enabled`** — 상태머신 전이 + reset 감지 훅. CONNECTED→ENABLING→ENABLED 승격, PCIe reset 특수 경로(outstanding abort), queued_req flush, NVMe-oF disconnect 경로
- **★ abort 계열 3종** (`abort_queued_reqs`, `_complete_abort_queued_reqs`, `abort_queued_reqs_with_cbarg`) — SWAP-기반 무한 재귀 회피 기법 상세 설명
- **★ `resubmit_requests`** — 완료 개수만큼 pop하여 재제출하는 flow control 원리
- **★ `complete_register_operations`** — multi-process에서 "요청한 프로세스의 cb_fn만 해당 프로세스에서 실행" 규약
- **11개 디버그 프린터** — nvme_get_sgl_*/prp/dptr/admin_qpair_command/io_qpair_command 전부, SCT 분기 디코딩 상세
- **11개 opcode/status 사전 테이블** — sentinel 규약, 테이블 선택 기준
- **`nvme_qpair_init`** — 64B align req_buf 풀 배치, reserved_req 특수 슬롯(Fabrics CONNECT 크리티컬 경로) 목적 설명
- **error injection 2종** — add/remove_cmd_error_injection + admin queue lock 규약

이로써 SPDK NVMe 드라이버 이해의 **두 기둥**이 완성: (1) nvme_ns_cmd.c에서 API → SQE 번역, (2) nvme_qpair.c에서 SQE → 트랜스포트 submit + CQE → cb_fn. 다음 세션은 `lib/nvme/nvme_pcie_common.c` (doorbell ring, PRP/SGL 빌드) 또는 `lib/nvme/nvme_transport.c` (트랜스포트 vtable 디스패치)로 이어간다.

---

**2026-04-24 (열여덟 번째 파트 — lib/nvme/nvme_ns_cmd.c 완전 완료)**: 이전 세션에서 핵심만 주석했던 `lib/nvme/nvme_ns_cmd.c`의 **29개 공개 API 래퍼 전부 주석 완료**. 이 파일은 이제 ☑ 완료 상태.

이번 세션에서 완료한 주요 그룹:
1. **Compare 4종** — NVMe fused CAS(Compare+Write atomic)의 전반부 진입점. CONTIG/CONTIG+MD/SGL/SGL+MD 4변형 모두 동일 파이프라인(opc=COMPARE만 다름)이지만 각각의 payload 구성 + PI apptag/mask 전파를 상세 문서화.

2. **Read/Write 변형 일괄** — 기존 read/write 핵심에 대응하는 with_md (separate metadata), v (SGL), v_with_md (SGL+MD), _ext (extended options), v_ext (SGL+ext) 전 조합. ★ 특히 `nvme_ns_cmd_rw_ext`(CONTIG)와 `nvme_ns_cmd_rwv_ext`(SGL) 내부 빌더는 accel_sequence/memory_domain/cdw13/PI mask 등 bdev_nvme가 세밀 제어하는 모든 옵션의 통합 진입점임을 명시.

3. **Zone Append 3종** — ★ ZNS의 핵심 기능. check_zone_append의 2단계 검증(ZONE_APPEND_SUPPORTED 비트 + ZASL 크기) + CONTIG/SGL 두 변형 각각에서 split 금지 방어(num_children 확인 후 free) + log-structured 자료구조와의 연결(장치가 wp 자동 할당 반환) 설명.

4. **관리형 I/O 6종** — write_zeroes/verify/write_uncorrectable/DSM/copy/flush. 이들은 _nvme_ns_cmd_rw를 거치지 않고 SQE를 직접 조립하는 "capability 명령" 카테고리. 각각:
   - write_zeroes (0x08): 장치 offload로 0-fill, PCIe 대역 절약
   - verify (0x0C): 미디어 스크러빙
   - write_uncorrectable (0x04): 보안/진단용 블록 마킹
   - DSM (0x09): discard/TRIM + sequential/latency 힌트
   - copy/SCC (0x19, NVMe 2.0): 장치 내부 복사 오프로드
   - flush (0x00): VWC → 비휘발성 매체 플러시

5. **Reservation 4종** — NVMe-oF / shared SSD / clustered FS에서 필수. 그룹 상단에 통합 설명(SCSI PR과의 유사성, 4개 커맨드 조합 워크플로우) + 각 함수에서 RREGA/RRELA/RACQA action 값별 의미, RTYPE 변형, CPTPL(power loss persist), IEKEY(key 검증 무시) 동작 모두 명시.

6. **IO Management 2종** — NVMe 2.0 TP4100 관리 서브커맨드 프레임워크. MO/MOS 필드 구조와 FDP(Flexible Data Placement) 사용처 설명.

**결과**: `lib/nvme/nvme_ns_cmd.c`를 읽으면 SPDK 유저스페이스 NVMe I/O 경로의 **호스트 측 모든 번역 로직**이 파일 하나로 완결 추적 가능. 다음 세션은 nvme_qpair.c(submit/complete)나 nvme_pcie_common.c(doorbell/PRP 빌드)로 이어갈 예정.

---

**2026-04-22 (열일곱 번째 파트 — lib/nvme/nvme_ns_cmd.c 구현 진입)**: **I/O 경로의 실제 코드로 첫 진입**. 이전까지 헤더·구조체 정의만 주석했다면 이제 사용자 API가 실제로 어떻게 `nvme_request`로 변환되는지 구현 레벨에서 추적 가능해졌다.

핵심 완료:
- 파일 상단 4섹션 블록 — 이 파일의 번역 레이어 역할, 호출 체인(user API → _nvme_ns_cmd_rw → split → setup_request → submit), 지원되는 모든 NVMe 명령 목록
- **`_nvme_ns_cmd_setup_request`** — SQE 필드 매핑 상세 주석 (CDW10-11의 64비트 SLBA alias 트릭, CDW12의 NLB 0-based 표기, CDW14의 PI Type1/2 RefTag 세팅, CDW15의 AppTag+mask 비트 배치)
- **`_nvme_ns_cmd_rw`** — 모든 R/W의 공통 엔트리. request 할당 → zone_append 예외 처리 → stripe split 조건(경계 가로지르는 I/O) → MDTS split → SGL/PRP split 분기 → setup_request
- split 보조 함수들 — `_nvme_add_child_request`(parent 롤백 포함), `_nvme_ns_cmd_split_request`(stripe/MDTS sector_mask 기반 분할)
- 파라미터 검증 — `_is_io_flags_valid`, `_is_accel_sequence_valid`(poll_group 연결 필수 이유 설명)
- 메타데이터 처리 — `_nvme_md_excluded_from_xfer`(PRACT + extended LBA + PI + md_size==8 4조건), `_nvme_get_host_buffer_sector_size`(sector vs extended_lba_size 분기)
- 에러 정제 — `nvme_ns_map_failure_rc`(-ENOMEM을 retry 가능 vs qdepth 초과로 영구 실패 분류)
- **`spdk_nvme_ns_cmd_read/write`** — 공개 API 진입점. 호스트 → 장치 전체 제출 순서(SQE 빌드 → tracker → PRP → doorbell MMIO → 장치 fetch → CQE → 콜백)를 상세 문서화.

이로써 **애플리케이션이 spdk_nvme_ns_cmd_read를 호출할 때 내부에서 일어나는 모든 번역 단계**가 주석만으로 추적 가능. 특히 "왜 64비트 SLBA를 *(uint64_t *)&cmd->cdw10에 직접 쓰는지"처럼 코드만 보면 의아한 부분들도 이유 명시.

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
