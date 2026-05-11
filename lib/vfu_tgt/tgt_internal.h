/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] vfu_tgt(vfio-user 타깃) 내부 헤더 (tgt_internal.h)
 *
 * === 파일의 역할 ===
 * SPDK는 PCIe 디바이스를 유저스페이스에서 에뮬레이트해 QEMU 등 다른 프로세스에 노출시키는
 * vfio-user 프로토콜의 "타깃 측" 프레임워크(lib/vfu_tgt)를 제공한다. 본 헤더는
 * vfio-user 엔드포인트(가상 PCIe 함수 1개를 표현하는 단위)의 내부 표현을 정의한다.
 * 외부 공개 API는 include/spdk/vfu_target.h에 있고, 본 파일은 코어 구현(tgt.c)과
 * RPC 핸들러(tgt_rpc.c) 사이에서 공유되는 struct spdk_vfu_endpoint를 단 한 번
 * 정의하는 단일 진실 소스이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름:
 *   QEMU(or 다른 클라이언트) ── Unix 도메인 소켓 ──→ libvfio-user(vfu_ctx_t)
 *     → SPDK vfu_tgt poller(accept_poller / vfu_ctx_poller)
 *       → struct spdk_vfu_endpoint(이 파일) ── ops 콜백 ──→
 *         endpoint_ctx의 도메인 모듈 (예: nvmf vfio_user transport, virtio-blk 에뮬)
 *           → SPDK bdev/nvmf 등 상위 레이어
 * SPDK reactor 위에서 동작하며, 각 endpoint는 한 SPDK thread에 고정된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/vfu_target.h(공개 ops/콜백 시그니처), libvfio-user(vfu_ctx_t,
 *   vfu_pci_config_space_t, msixcap), spdk/thread(spdk_thread/spdk_poller),
 *   sys/queue.h(TAILQ).
 * - 의존받음: lib/vfu_tgt/tgt.c (라이프사이클·polling), lib/vfu_tgt/tgt_rpc.c
 *   (소켓 베이스 경로 RPC), module/vfu_device/* 의 하위 디바이스 모듈들.
 * - 데이터 흐름: 클라이언트가 PCIe MMIO/Config/MSI-X 접근을 요청하면 libvfio-user가
 *   콜백을 호출 → endpoint->ops 디스패치 → endpoint_ctx의 도메인 모듈이 처리.
 * - 공유 자료구조: 글로벌 endpoint TAILQ(tgt.c의 g_endpoint_list)에 본 구조체가 link로 연결.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_vfu_endpoint : 한 vfio-user 엔드포인트(=1 가상 PCIe 함수)의 내부 표현.
 *   - name/uuid : 식별자와 UDS 경로 산출 키.
 *   - ops/endpoint_ctx : 도메인 모듈 콜백 + 그 모듈이 소유한 컨텍스트.
 *   - vfu_ctx : libvfio-user의 PCIe 디바이스 핸들.
 *   - accept_poller/vfu_ctx_poller : SPDK poller 2종(연결 수락 / 메시지 처리).
 *   - msix/pci_config_space : MSI-X capability와 PCI Config Space 바인딩.
 *   - thread : 본 endpoint가 고정된 SPDK thread (cross-thread 호출 금지 단서).
 *   - link : 전역 endpoint 리스트의 TAILQ 엔트리.
 */

#ifndef _TGT_INTERNAL_H
#define _TGT_INTERNAL_H
/* [한국어] 다중 포함 가드 — 본 헤더가 같은 TU에서 두 번 이상 include되어도 안전하도록. */

#include "spdk/vfu_target.h"
/* [한국어] 공개 API 헤더 — SPDK_VFU_MAX_NAME_LEN 매크로와
 * struct spdk_vfu_endpoint_ops, vfu_ctx_t 전방 타입 가시화. */

struct spdk_vfu_endpoint {
	/* [한국어] vfio-user 엔드포인트(=1 가상 PCIe 함수)의 내부 상태 컨테이너.
	 * 한 SPDK thread 소유로, 해당 thread의 reactor 위에서만 필드가 변경된다.
	 * 외부에서는 spdk_vfu_endpoint_* 공개 API로만 접근한다. */

	char				name[SPDK_VFU_MAX_NAME_LEN];
	/* [한국어] 엔드포인트 식별 문자열(예: "nvme0", "blk1") — 유저가 RPC로 지정.
	 * 설정자: spdk_vfu_create_endpoint() 시 strncpy로 채움.
	 * 읽는 자: 로그 출력, 전역 리스트 탐색, RPC 응답.
	 * 값 범위: NUL 종결 ASCII, 최대 SPDK_VFU_MAX_NAME_LEN-1 글자.
	 * 동기화: thread 고정이므로 endpoint 소유 thread에서만 안전. */

	char				uuid[PATH_MAX];
	/* [한국어] vfio-user Unix 도메인 소켓 경로 (base_path/name 형태로 합성).
	 * 설정자: 생성 시 g_socket_path + name으로 조합.
	 * 읽는 자: vfu_create_ctx()에 전달되어 listen 소켓 경로로 사용.
	 * 값 범위: 절대 경로 문자열, PATH_MAX(=4096) 미만.
	 * 동기화: 변경 없음(생성 후 immutable). */

	struct spdk_vfu_endpoint_ops	ops;
	/* [한국어] 도메인 모듈이 제공한 콜백 테이블의 사본(예: get_device_info, attach_device,
	 * pci_config_*, post_memory_add 등). libvfio-user 이벤트가 들어올 때 SPDK 측에서
	 * 디스패치할 함수 포인터들을 담는다.
	 * 설정자: 모듈 등록 시 ops 구조체 전체 복사.
	 * 읽는 자: poller가 PCIe/MSI-X/메모리 이벤트마다 해당 ops.* 호출.
	 * 값 범위: 모든 필수 콜백이 non-NULL이어야 함.
	 * 동기화: endpoint thread에서만 호출되므로 별도 lock 불필요. */

	vfu_ctx_t			*vfu_ctx;
	/* [한국어] libvfio-user가 반환한 디바이스 컨텍스트 핸들 — vfio-user 프로토콜의 핵심 객체.
	 * 설정자: vfu_create_ctx() 성공 시 저장.
	 * 읽는 자: vfu_ctx_poller가 vfu_run_ctx()로 메시지 처리, 종료 시 vfu_destroy_ctx().
	 * 값 범위: libvfio-user의 불투명 포인터 — 직접 멤버 접근 금지.
	 * 동기화: libvfio-user 내부에서 single-thread 사용 가정. */

	void				*endpoint_ctx;
	/* [한국어] 도메인 모듈이 자체 상태(예: nvmf vfio_user transport의 ctrlr)를 저장하는 슬롯.
	 * 설정자: ops.attach_device 또는 모듈 생성자에서 모듈이 직접 채움.
	 * 읽는 자: ops 콜백이 첫 인자로 자기 ctx를 받아 모듈별 상태에 접근.
	 * 값 범위: 모듈이 정의한 임의 타입 — 본 코어는 그 내용을 해석하지 않음.
	 * 동기화: endpoint thread 소유. */

	struct spdk_poller		*accept_poller;
	/* [한국어] vfio-user 클라이언트(QEMU 등)의 Unix 소켓 연결 수락을 polling하는 SPDK poller.
	 * 설정자: 엔드포인트 생성 직후 SPDK_POLLER_REGISTER로 등록.
	 * 읽는 자: SPDK reactor 메인 루프가 주기적으로 호출.
	 * 값 범위: spdk_poller 핸들 (NULL이면 미등록).
	 * 동기화: endpoint thread에 묶여 있어 동일 thread에서만 trigger됨. */

	struct spdk_poller		*vfu_ctx_poller;
	/* [한국어] 연결 수립(attach) 후 클라이언트로부터의 메시지(MMIO/Config 액세스 등)를
	 * 처리하는 poller — vfu_run_ctx()를 반복 호출.
	 * 설정자: attach 완료 시 등록, detach 시 해제.
	 * 읽는 자: SPDK reactor 메인 루프.
	 * 값 범위: spdk_poller 핸들 또는 NULL.
	 * 동기화: endpoint thread 고정. */

	bool				is_attached;
	/* [한국어] 클라이언트가 attach된 상태인지 여부 (true=연결됨).
	 * 설정자: accept_poller가 새 연결 수락 시 true, detach 콜백 시 false.
	 * 읽는 자: poller 분기, 종료 시퀀스에서의 안전한 cleanup 결정.
	 * 값 범위: true/false.
	 * 동기화: endpoint thread만 read/write — atomic 불필요. */

	struct msixcap			*msix;
	/* [한국어] PCIe MSI-X capability 구조체 포인터 (libvfio-user가 PCI Config Space 안에 마련).
	 * 설정자: PCI config 초기화 시 vfu_pci_get_config_space()의 결과 내부 오프셋을 가리키도록 설정.
	 * 읽는 자: 모듈이 인터럽트(MSI-X 벡터) 발사 시 해당 capability 비트(MSI-X enable/mask) 검사.
	 * 값 범위: pci_config_space 내부 오프셋 — 동일 영역의 일부 포인터.
	 * 동기화: 클라이언트의 config write도 ops.pci_config_*를 통해 endpoint thread로 직렬화. */

	vfu_pci_config_space_t		*pci_config_space;
	/* [한국어] PCI Configuration Space 256/4KB 영역 미러 — vendor/device ID, BAR, capability list 등.
	 * 설정자: vfu_pci_get_config_space()로 획득, 모듈이 ID/BAR 설정 시 직접 write.
	 * 읽는 자: 클라이언트 config read는 libvfio-user가 이 영역에서 그대로 응답.
	 * 값 범위: PCI 스펙(PCI Local Bus 3.0, PCIe Base Spec)의 Config Header + Capability 체인.
	 * 동기화: 초기화 후엔 read-only가 일반적. write가 필요한 시점은 endpoint thread에서. */

	struct spdk_thread		*thread;
	/* [한국어] 본 endpoint가 고정(affinity)된 SPDK thread 핸들 — 모든 ops/poller가 여기서 실행.
	 * 설정자: 생성 시 spdk_get_thread()로 캡처(보통 마스터 또는 사용자 지정 thread).
	 * 읽는 자: cross-thread 작업이 필요할 때 spdk_thread_send_msg(thread, ...)로 전달.
	 * 값 범위: 유효한 spdk_thread 핸들 (NULL 불가).
	 * 동기화: 본 필드 자체는 immutable이고, "이 thread에서만 endpoint를 만지라"는 계약의 단서. */

	TAILQ_ENTRY(spdk_vfu_endpoint)	link;
	/* [한국어] 전역 endpoint 리스트(tgt.c의 g_endpoint_list — TAILQ_HEAD)에 연결되는 링크.
	 * 설정자: 생성 시 TAILQ_INSERT_TAIL, 파괴 시 TAILQ_REMOVE.
	 * 읽는 자: 이름으로 endpoint 검색, 전역 정리 시 순회.
	 * 값 범위: sys/queue.h가 제공하는 prev/next 포인터 한 쌍.
	 * 동기화: 전역 리스트는 마스터 thread에서 일관되게 변경됨(혹은 RPC thread). */
};

#endif
/* [한국어] 다중 포함 가드 종결 (#ifndef _TGT_INTERNAL_H 짝). */
