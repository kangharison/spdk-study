/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2025, Oracle and/or its affiliates.
 */

/*
 * [한국어 설명] NVMe-oF Subsystem 코어 구현 (subsystem.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPDK NVMe-over-Fabrics (NVMe-oF) target의 핵심 단위인
 * "subsystem"을 구현한다. NVMe-oF에서 subsystem이란 NQN(NVMe Qualified Name)으로
 * 식별되는 논리적 NVMe target으로서, 다음을 캡슐화한다:
 *   - 여러 namespace(NS) 컬렉션 (각 NS는 backing bdev에 매핑됨)
 *   - 호스트(initiator) 접근 제어 리스트 (allow_any_host / 명시적 hostnqn 화이트리스트)
 *   - listener 리스트 (이 subsystem을 노출할 transport 주소: TCP/RDMA/FC의 IP:port)
 *   - 컨트롤러(ctrlr) 리스트 (호스트가 이 subsystem에 connect 하면 ctrlr 인스턴스 생성)
 *   - SN(Serial Number), MN(Model Number), 컨트롤러 ID 범위 등 스펙상 식별 정보
 *   - ANA(Asymmetric Namespace Access) 그룹 상태 머신
 *   - PR(Persistent Reservation) 처리 콜백/등록자 리스트
 * NQN 검증, subsystem CRUD, host/listener/NS 추가·제거, 비동기 상태 머신
 * (INACTIVE → ACTIVATING → ACTIVE → PAUSING → PAUSED → RESUMING → ACTIVE)
 * 의 모든 전이를 이 파일이 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe-oF target은 크게 다음 계층으로 구성된다:
 *   spdk_nvmf_tgt (전역 target)  ─ 여러 subsystem 보유
 *      └── spdk_nvmf_subsystem    ─ 본 파일 관리 대상 (여러 NS + 여러 listener + 여러 ctrlr)
 *            ├── spdk_nvmf_ns      ─ bdev 매핑된 NS
 *            ├── spdk_nvmf_subsystem_listener ─ transport 주소 등록
 *            └── spdk_nvmf_ctrlr   ─ 호스트당 컨트롤러 인스턴스 (ctrlr.c에서 정의)
 *      └── spdk_nvmf_transport     ─ TCP/RDMA/FC transport (transport.c)
 *            └── spdk_nvmf_qpair    ─ 호스트와의 큐 페어 연결
 *      └── spdk_nvmf_poll_group    ─ per-CPU poll group (poll_group.c)
 * 본 파일은 control plane (관리/구성) 측면을 담당하며, 실제 NVMe 명령
 * 디스패치(IO/admin queue 처리)는 ctrlr.c, 데이터 plane은 각 transport
 * 모듈(tcp.c, rdma.c, fc.c)에서 처리된다.
 *
 * 실행 컨텍스트: 대부분의 함수는 SPDK app thread (마스터 reactor) 또는
 * 호출자가 지정한 thread에서 호출된다. 상태 전이 함수는 모든 poll group을
 * 비동기적으로 순회하면서(spdk_for_each_channel) 적용 후 콜백으로 완료를
 * 알리는 패턴을 사용한다. 이 패턴이 lockless 설계의 근간이다.
 *
 * === 타 모듈과의 연결 ===
 * - nvmf.c: spdk_nvmf_tgt 라이프사이클 관리; subsystem을 tgt에 등록/해제
 * - ctrlr.c: 컨트롤러 생성/관리, NVMe admin/IO 명령 처리; subsystem 상태와
 *   host ACL을 참조한다
 * - transport.c, tcp.c, rdma.c, fc.c: listener를 등록할 때 transport에
 *   listen 요청 위임; subsystem 상태 변화 시 각 transport poll group에 통지
 * - lib/bdev: NS가 bdev_open으로 열리고 spdk_bdev_io_complete 콜백을 통해
 *   write/read 결과 수신
 * - PR(Persistent Reservation): 디폴트 reservation 처리는 본 파일의
 *   nvmf_ns_reservation_xxx 정적 함수가 담당하며, 외부에서 g_reservation_ops
 *   교체로 커스텀 가능
 *
 * 데이터 흐름:
 *   spdk_nvmf_subsystem_create → tgt에 등록 → add_ns(bdev open) →
 *   add_listener(transport listen) → add_host(ACL) → start(상태 ACTIVE)
 *   → 호스트 connect → ctrlr_create → admin/IO 명령 처리 → ...
 *
 * === 주요 함수/구조체 요약 ===
 * 공개 API (라이프사이클):
 *   - spdk_nvmf_subsystem_create: NQN 검증 + subsystem 객체 할당 + tgt 등록
 *   - spdk_nvmf_subsystem_destroy: ctrlr/NS/listener/host 모두 제거 후 해제
 *   - spdk_nvmf_subsystem_start/stop/pause/resume: 상태 머신 전이 트리거
 *   - spdk_nvmf_subsystem_add_ns_ext / remove_ns: NS 등록/해제
 *   - spdk_nvmf_subsystem_add_listener_ext / remove_listener: transport 주소 등록/해제
 *   - spdk_nvmf_subsystem_add_host_ext / remove_host: 호스트 ACL 관리
 *   - spdk_nvmf_subsystem_set_ana_state: ANA 그룹 상태 변경 (멀티패스 지원)
 *
 * 핵심 정적 함수:
 *   - nvmf_nqn_is_valid: RFC 1034 도메인 + NVMe-oF NQN 형식 검증
 *   - nvmf_subsystem_set_state: atomic CAS로 상태 전이 (lockless)
 *   - nvmf_subsystem_state_change_on_pg: per-poll-group 상태 변경 적용
 *   - nvmf_ns_reservation_xxx: PR-IN/OUT 명령 처리 (PRECONDITIONS, types)
 *
 * 핵심 자료구조 (struct는 nvmf_internal.h에 정의):
 *   - spdk_nvmf_subsystem: NQN, state, NS 배열, host/listener/ctrlr 리스트
 *   - spdk_nvmf_ns: NSID, bdev_desc, ANA group, reservation 상태
 *   - spdk_nvmf_subsystem_listener: trid, ANA state per group, transport-specific opts
 *   - spdk_nvmf_host: hostnqn ACL 엔트리
 */

#include "spdk/stdinc.h"			/* [한국어] 표준 C 헤더 모음 (stdint, stddef, string 등) */

#include "nvmf_internal.h"			/* [한국어] NVMe-oF 내부 자료구조 (subsystem/ctrlr/ns 정의) */
#include "transport.h"				/* [한국어] transport 레이어 추상화 인터페이스 */

#include "spdk/assert.h"			/* [한국어] SPDK_STATIC_ASSERT/CONTAINEROF 매크로 */
#include "spdk/likely.h"			/* [한국어] spdk_likely/spdk_unlikely 분기 힌트 */
#include "spdk/string.h"			/* [한국어] spdk_strerror 등 문자열 유틸 */
#include "spdk/trace.h"				/* [한국어] SPDK trace point 등록/기록 매크로 */
#include "spdk/nvmf_spec.h"			/* [한국어] NVMe-oF 와이어 스펙 (PDU/Capsule 정의) */
#include "spdk/uuid.h"				/* [한국어] UUID 파싱/생성 (NQN UUID 형식 검증에 사용) */
#include "spdk/json.h"				/* [한국어] JSON 직렬화 (PR 영속화 시 활용) */
#include "spdk/file.h"				/* [한국어] 파일 I/O 유틸 (PR 상태 파일 영속화) */
#include "spdk/bit_array.h"			/* [한국어] cntlid/NSID 할당 추적용 비트맵 */
#include "spdk/bdev.h"				/* [한국어] bdev API (NS의 backing storage 조작) */

#define __SPDK_BDEV_MODULE_ONLY			/* [한국어] bdev_module.h를 모듈 측면만 노출 (외부 API 충돌 방지) */
#include "spdk/bdev_module.h"			/* [한국어] bdev module 등록 (ns_bdev_module 정의용) */
#include "spdk/log.h"				/* [한국어] SPDK_ERRLOG/INFOLOG/DEBUGLOG 매크로 */
#include "spdk_internal/utf.h"			/* [한국어] UTF-8 검증 (Model/Serial Number 문자열 체크) */
#include "spdk_internal/usdt.h"			/* [한국어] USDT(User Statically Defined Tracing) 마커 */

#define MODEL_NUMBER_DEFAULT "SPDK bdev Controller"
/* [한국어] NVMe Identify Controller 응답의 MN(Model Number, 40 byte ASCII) 기본값.
 * 호스트는 nvme list 등에서 이 문자열을 모델명으로 표시한다. */

#define NVMF_SUBSYSTEM_DEFAULT_NAMESPACES 32
/* [한국어] subsystem 생성 시 사용자가 max_namespaces=0을 주면 적용되는 NS 슬롯 기본값.
 * NS 배열(subsystem->ns)을 미리 할당하기 위한 상한; 이후 add_ns로 채워간다. */

/*
 * States for parsing valid domains in NQNs according to RFC 1034
 */
/* [한국어] NQN 검증 상태 머신.
 * NVMe-oF NQN의 reverse-domain 부분(예: "org.nvmexpress")을 RFC 1034 도메인
 * 라벨 규칙(letter로 시작, letter/digit/hyphen 중간, letter/digit로 끝)으로
 * 한 글자씩 검증하는 동안 사용. nvmf_nqn_is_valid()의 switch에서 이 상태를 전이. */
enum spdk_nvmf_nqn_domain_states {
	/* First character of a domain must be a letter */
	SPDK_NVMF_DOMAIN_ACCEPT_LETTER = 0,
	/* [한국어] 라벨의 첫 글자 위치. 반드시 알파벳이어야 한다 (RFC 1034). */

	/* Subsequent characters can be any of letter, digit, or hyphen */
	SPDK_NVMF_DOMAIN_ACCEPT_LDH = 1,
	/* [한국어] 라벨 중간 위치. Letter/Digit/Hyphen(LDH) 모두 허용.
	 * 단, 하이픈으로 라벨이 끝나서는 안 된다 (다음 분기에서 검증). */

	/* A domain label must end with either a letter or digit */
	SPDK_NVMF_DOMAIN_ACCEPT_ANY = 2
	/* [한국어] 직전 글자가 letter/digit이라 라벨 종료 가능한 상태.
	 * '.'(다음 라벨로 전이), '-'(LDH로 전이), letter/digit(자기 유지) 모두 허용. */
};

/* [한국어] 자기 참조 forward declaration: subsystem 비동기 destroy의 내부 단계용.
 * spdk_nvmf_subsystem_destroy → 모든 ctrlr disconnect 완료 콜백 →
 * _nvmf_subsystem_destroy → 실제 메모리 해제 순서로 호출된다. */
static int _nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem);

/* Returns true if is a valid ASCII string as defined by the NVMe spec */
/*
 * [한국어]
 * nvmf_valid_ascii_string - NVMe 스펙 호환 ASCII 문자열 검증
 *
 * @buf:  검증할 바이트 버퍼 (Identify Controller의 SN/MN/FR 등 고정 크기 필드)
 * @size: 버퍼 크기 (NVMe 스펙상 SN=20, MN=40, FR=8 등)
 * @return: 모든 바이트가 0x20(SPACE)~0x7E(~) 범위면 true, 하나라도 벗어나면 false
 *
 * NVMe 1.x 스펙은 컨트롤러 식별 문자열을 "ASCII printable characters"로
 * 제한한다 (제어문자/non-ASCII 금지). 호스트가 nvme list 등에서 표시할 때
 * 깨진 문자가 나타나는 것을 방지한다.
 *
 * 실행 컨텍스트: spdk_nvmf_subsystem_set_sn/set_mn 등 관리 API에서 호출.
 * 동기화: read-only 검증이라 락 불필요.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_set_sn/set_mn → [nvmf_valid_ascii_string]
 */
static bool
nvmf_valid_ascii_string(const void *buf, size_t size)
{
	const uint8_t *str = buf;	/* [한국어] uint8_t 캐스팅 — 부호 비교 회피 (signed char 음수값 방지) */
	size_t i;			/* [한국어] 루프 인덱스 */

	for (i = 0; i < size; i++) {				/* [한국어] 모든 바이트 순회 */
		if (str[i] < 0x20 || str[i] > 0x7E) {		/* [한국어] printable ASCII 범위 밖이면 즉시 거부 */
			return false;				/* [한국어] 제어문자(0x00~0x1F, 0x7F) 또는 비 ASCII(>=0x80) 포함 */
		}
	}

	return true;						/* [한국어] 전 구간 합법 ASCII */
}

/*
 * [한국어]
 * nvmf_nqn_is_valid - NVMe-oF NQN(NVMe Qualified Name) 형식 검증
 *
 * @nqn: 검증할 NQN 문자열 (NULL 종결)
 * @return: 형식 적합 시 true, 부적합 시 false (이때 SPDK_ERRLOG로 사유 출력)
 *
 * NVMe-oF 스펙(1.0 Section 7.9 등)은 NQN을 다음 두 가지 형식으로 정의한다:
 *   1) Discovery NQN: "nqn.2014-08.org.nvmexpress.discovery"
 *   2) UUID 기반:     "nqn.2014-08.org.nvmexpress:uuid:<uuid string>"
 *   3) 일반 형식:     "nqn.YYYY-MM.<reverse domain>:<user string>"
 * 길이 범위(11~223), reverse-domain RFC 1034 검증, 콜론 이후 user-string
 * 존재 등 여러 단계를 거친다.
 *
 * 호출 컨텍스트: spdk_nvmf_subsystem_create 진입점에서 호출되어 잘못된
 * NQN으로 subsystem이 생성되는 것을 차단한다. 또 호스트 add 시 hostnqn에
 * 대해서도 호출.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_create → [nvmf_nqn_is_valid]
 *   spdk_nvmf_subsystem_add_host_ext → [nvmf_nqn_is_valid]
 */
bool
nvmf_nqn_is_valid(const char *nqn)
{
	size_t len;
	struct spdk_uuid uuid_value;
	uint32_t i;
	int bytes_consumed;
	uint32_t domain_label_length;
	char *reverse_domain_end;
	uint32_t reverse_domain_end_index;
	enum spdk_nvmf_nqn_domain_states domain_state = SPDK_NVMF_DOMAIN_ACCEPT_LETTER;

	/* Check for length requirements */
	len = strlen(nqn);
	if (len > SPDK_NVMF_NQN_MAX_LEN) {
		SPDK_ERRLOG("Invalid NQN \"%s\": length %zu > max %d\n", nqn, len, SPDK_NVMF_NQN_MAX_LEN);
		return false;
	}

	/* The nqn must be at least as long as SPDK_NVMF_NQN_MIN_LEN to contain the necessary prefix. */
	if (len < SPDK_NVMF_NQN_MIN_LEN) {
		SPDK_ERRLOG("Invalid NQN \"%s\": length %zu < min %d\n", nqn, len, SPDK_NVMF_NQN_MIN_LEN);
		return false;
	}

	/* Check for discovery controller nqn */
	if (!strcmp(nqn, SPDK_NVMF_DISCOVERY_NQN)) {
		return true;
	}

	/* Check for equality with the generic nqn structure of the form "nqn.2014-08.org.nvmexpress:uuid:11111111-2222-3333-4444-555555555555" */
	if (!strncmp(nqn, SPDK_NVMF_NQN_UUID_PRE, SPDK_NVMF_NQN_UUID_PRE_LEN)) {
		if (len != SPDK_NVMF_NQN_UUID_PRE_LEN + SPDK_NVMF_UUID_STRING_LEN) {
			SPDK_ERRLOG("Invalid NQN \"%s\": uuid is not the correct length\n", nqn);
			return false;
		}

		if (spdk_uuid_parse(&uuid_value, &nqn[SPDK_NVMF_NQN_UUID_PRE_LEN])) {
			SPDK_ERRLOG("Invalid NQN \"%s\": uuid is not formatted correctly\n", nqn);
			return false;
		}
		return true;
	}

	/* If the nqn does not match the uuid structure, the next several checks validate the form "nqn.yyyy-mm.reverse.domain:user-string" */

	if (strncmp(nqn, "nqn.", 4) != 0) {
		SPDK_ERRLOG("Invalid NQN \"%s\": NQN must begin with \"nqn.\".\n", nqn);
		return false;
	}

	/* Check for yyyy-mm. */
	if (!(isdigit(nqn[4]) && isdigit(nqn[5]) && isdigit(nqn[6]) && isdigit(nqn[7]) &&
	      nqn[8] == '-' && isdigit(nqn[9]) && isdigit(nqn[10]) && nqn[11] == '.')) {
		SPDK_ERRLOG("Invalid date code in NQN \"%s\"\n", nqn);
		return false;
	}

	reverse_domain_end = strchr(nqn, ':');
	if (reverse_domain_end != NULL && (reverse_domain_end_index = reverse_domain_end - nqn) < len - 1) {
	} else {
		SPDK_ERRLOG("Invalid NQN \"%s\". NQN must contain user specified name with a ':' as a prefix.\n",
			    nqn);
		return false;
	}

	/* Check for valid reverse domain */
	domain_label_length = 0;
	for (i = 12; i < reverse_domain_end_index; i++) {
		if (domain_label_length > SPDK_DOMAIN_LABEL_MAX_LEN) {
			SPDK_ERRLOG("Invalid domain name in NQN \"%s\". At least one Label is too long.\n", nqn);
			return false;
		}

		switch (domain_state) {

		case SPDK_NVMF_DOMAIN_ACCEPT_LETTER: {
			if (isalpha(nqn[i])) {
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_ANY;
				domain_label_length++;
				break;
			} else {
				SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must start with a letter.\n", nqn);
				return false;
			}
		}

		case SPDK_NVMF_DOMAIN_ACCEPT_LDH: {
			if (isalpha(nqn[i]) || isdigit(nqn[i])) {
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_ANY;
				domain_label_length++;
				break;
			} else if (nqn[i] == '-') {
				if (i == reverse_domain_end_index - 1) {
					SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must end with an alphanumeric symbol.\n",
						    nqn);
					return false;
				}
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_LDH;
				domain_label_length++;
				break;
			} else if (nqn[i] == '.') {
				SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must end with an alphanumeric symbol.\n",
					    nqn);
				return false;
			} else {
				SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must contain only [a-z,A-Z,0-9,'-','.'].\n",
					    nqn);
				return false;
			}
		}

		case SPDK_NVMF_DOMAIN_ACCEPT_ANY: {
			if (isalpha(nqn[i]) || isdigit(nqn[i])) {
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_ANY;
				domain_label_length++;
				break;
			} else if (nqn[i] == '-') {
				if (i == reverse_domain_end_index - 1) {
					SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must end with an alphanumeric symbol.\n",
						    nqn);
					return false;
				}
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_LDH;
				domain_label_length++;
				break;
			} else if (nqn[i] == '.') {
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_LETTER;
				domain_label_length = 0;
				break;
			} else {
				SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must contain only [a-z,A-Z,0-9,'-','.'].\n",
					    nqn);
				return false;
			}
		}
		}
	}

	i = reverse_domain_end_index + 1;
	while (i < len) {
		bytes_consumed = utf8_valid(&nqn[i], &nqn[len]);
		if (bytes_consumed <= 0) {
			SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must contain only valid utf-8.\n", nqn);
			return false;
		}

		i += bytes_consumed;
	}
	return true;
}

/* [한국어] forward decl: spdk_for_each_channel 콜백.
 * 상태 전이 시 모든 poll group(per-CPU)을 순회하며 적용하기 위한 진입점. */
static void subsystem_state_change_on_pg(struct spdk_io_channel_iter *i);

/*
 * [한국어]
 * spdk_nvmf_subsystem_create - NVMe-oF subsystem 객체 생성 및 tgt에 등록
 *
 * @tgt:    소속될 nvmf target (전역 컨테이너)
 * @nqn:    이 subsystem의 NQN 식별자 (NVMe-oF 스펙 형식)
 * @type:   subsystem 유형 (NVME=일반 / DISCOVERY=호스트가 listener 목록을 조회)
 * @num_ns: 사전 할당할 NS 슬롯 수 (0이면 기본값 32; discovery는 0 강제)
 * @return: 성공 시 subsystem 포인터, 실패 시 NULL (NQN 충돌/형식오류/메모리 부족)
 *
 * NVMe-oF subsystem은 호스트 시점에서 "원격 NVMe SSD" 단위로 보인다.
 * 본 함수는 다음을 한 트랜잭션처럼 수행한다:
 *   1) NQN 유일성/형식 검증
 *   2) tgt의 subsystem_ids 비트맵에서 빈 slot id 할당 (cntlid 부여 등에 사용)
 *   3) subsystem 객체 calloc, 멤버 초기화, NS 배열/ANA 그룹 calloc
 *   4) tgt의 RB tree에 삽입
 * 생성 직후 상태는 SPDK_NVMF_SUBSYSTEM_INACTIVE이며, NS/listener/host를
 * 모두 추가한 뒤 spdk_nvmf_subsystem_start로 ACTIVE 전이해야 호스트 connect
 * 수신을 시작한다.
 *
 * 실행 컨텍스트: 보통 RPC 콜백 또는 init 루틴에서 호출되며, 호출 thread가
 * 곧 subsystem->thread가 된다 (이후 모든 control 작업의 home thread).
 *
 * 호출 체인:
 *   RPC handler / init → [spdk_nvmf_subsystem_create] → spdk_nvmf_tgt_find_subsystem
 *     → nvmf_nqn_is_valid → spdk_bit_array_find_first_clear → calloc → RB_INSERT
 */
struct spdk_nvmf_subsystem *
spdk_nvmf_subsystem_create(struct spdk_nvmf_tgt *tgt,
			   const char *nqn,
			   enum spdk_nvmf_subtype type,
			   uint32_t num_ns)
{
	struct spdk_nvmf_subsystem	*subsystem;	/* [한국어] 새로 만들 subsystem 객체 */
	uint32_t			sid;		/* [한국어] 할당받을 subsystem id (tgt 내 유일) */

	if (spdk_nvmf_tgt_find_subsystem(tgt, nqn)) {	/* [한국어] 동일 NQN이 이미 등록되어 있으면 거부 (NQN은 tgt 내 유일) */
		SPDK_ERRLOG("Subsystem NQN '%s' already exists\n", nqn);
		return NULL;
	}

	if (!nvmf_nqn_is_valid(nqn)) {			/* [한국어] NVMe-oF/RFC 1034 형식 검증 — 부적합 시 거부 */
		SPDK_ERRLOG("Subsystem NQN '%s' is invalid\n", nqn);
		return NULL;
	}

	if (type == SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT ||
	    type == SPDK_NVMF_SUBTYPE_DISCOVERY) {	/* [한국어] discovery subsystem은 NS를 가질 수 없다 (스펙상 listener 광고만 함) */
		if (num_ns != 0) {
			SPDK_ERRLOG("Discovery subsystem cannot have namespaces.\n");
			return NULL;
		}
	} else if (num_ns == 0) {			/* [한국어] 일반 subsystem이고 num_ns가 0이면 기본값(32) 적용 */
		num_ns = NVMF_SUBSYSTEM_DEFAULT_NAMESPACES;
	}

	/* Find a free subsystem id (sid) */
	sid = spdk_bit_array_find_first_clear(tgt->subsystem_ids, 0);	/* [한국어] tgt 비트맵에서 가장 작은 미사용 id 검색 (lockless O(N/64)) */
	if (sid == UINT32_MAX) {					/* [한국어] 빈 슬롯 없음 — tgt 한도 초과 */
		SPDK_ERRLOG("No free subsystem IDs are available for subsystem creation\n");
		return NULL;
	}
	subsystem = calloc(1, sizeof(struct spdk_nvmf_subsystem));	/* [한국어] 0으로 초기화된 subsystem 메모리 할당 */
	if (subsystem == NULL) {
		SPDK_ERRLOG("Subsystem memory allocation failed\n");
		return NULL;
	}

	subsystem->thread = spdk_get_thread();		/* [한국어] 본 호출 thread를 home thread로 고정 — 이후 control 작업 위치 */
	subsystem->state = SPDK_NVMF_SUBSYSTEM_INACTIVE;	/* [한국어] 초기 상태: I/O 수신 불가, NS/listener 자유 변경 가능 */
	subsystem->tgt = tgt;				/* [한국어] 역참조용 부모 tgt 포인터 */
	subsystem->id = sid;				/* [한국어] tgt 내 유일한 정수 id (cntlid 베이스 등에 활용) */
	subsystem->subtype = type;			/* [한국어] NVME / DISCOVERY 구분 */
	subsystem->max_nsid = num_ns;			/* [한국어] 사전 할당된 NS 슬롯 수 (이후 ns 배열 인덱스 상한) */
	subsystem->next_cntlid = 1;			/* [한국어] 다음 컨트롤러에 부여할 cntlid 후보 (1부터 round-robin) */
	subsystem->min_cntlid = NVMF_MIN_CNTLID;	/* [한국어] cntlid 최소값 (스펙 1) */
	subsystem->max_cntlid = NVMF_MAX_CNTLID;	/* [한국어] cntlid 최대값 (스펙 0xFFEF) */
	snprintf(subsystem->subnqn, sizeof(subsystem->subnqn), "%s", nqn);	/* [한국어] NQN 문자열을 고정 크기 버퍼에 안전 복사 */
	pthread_mutex_init(&subsystem->mutex, NULL);	/* [한국어] hosts/state_changes 등 control plane 자료구조 보호용 mutex */
	TAILQ_INIT(&subsystem->listeners);		/* [한국어] subsystem이 노출되는 transport 주소 리스트 */
	TAILQ_INIT(&subsystem->hosts);			/* [한국어] 허용된 hostnqn ACL 리스트 (allow_any_host=false인 경우 의미) */
	TAILQ_INIT(&subsystem->ctrlrs);			/* [한국어] 호스트 connect로 생성된 ctrlr 인스턴스 리스트 */
	TAILQ_INIT(&subsystem->state_changes);		/* [한국어] 진행 중/대기 중인 상태 전이 요청 큐 (직렬화) */
	subsystem->used_listener_ids = spdk_bit_array_create(NVMF_MAX_LISTENERS_PER_SUBSYSTEM);	/* [한국어] listener id 할당 추적 비트맵 */
	if (subsystem->used_listener_ids == NULL) {	/* [한국어] 비트맵 할당 실패 — 부분 할당된 자원 정리 */
		pthread_mutex_destroy(&subsystem->mutex);
		free(subsystem);
		SPDK_ERRLOG("Listener id array memory allocation failed\n");
		return NULL;
	}

	if (num_ns != 0) {				/* [한국어] 일반 subsystem이라면 NS/ANA 배열 사전 할당 */
		subsystem->ns = calloc(num_ns, sizeof(struct spdk_nvmf_ns *));	/* [한국어] NSID(1~max_nsid) 인덱스용 포인터 배열 */
		if (subsystem->ns == NULL) {
			SPDK_ERRLOG("Namespace memory allocation failed\n");
			pthread_mutex_destroy(&subsystem->mutex);
			spdk_bit_array_free(&subsystem->used_listener_ids);
			free(subsystem);
			return NULL;
		}
		subsystem->ana_group = calloc(num_ns, sizeof(uint32_t));	/* [한국어] ANA 그룹 카운터 배열 (ANA 그룹 id별 NS 수 관리) */
		if (subsystem->ana_group == NULL) {
			SPDK_ERRLOG("ANA group memory allocation failed\n");
			pthread_mutex_destroy(&subsystem->mutex);
			free(subsystem->ns);
			spdk_bit_array_free(&subsystem->used_listener_ids);
			free(subsystem);
			return NULL;
		}
	}

	memset(subsystem->sn, '0', sizeof(subsystem->sn) - 1);	/* [한국어] SN(20 char) 디폴트는 '0' 채우기 (호스트가 set_sn 호출 전까지) */
	subsystem->sn[sizeof(subsystem->sn) - 1] = '\0';	/* [한국어] NULL 종결자 보장 */

	snprintf(subsystem->mn, sizeof(subsystem->mn), "%s",
		 MODEL_NUMBER_DEFAULT);				/* [한국어] MN(40 char) 디폴트 "SPDK bdev Controller" */

	spdk_bit_array_set(tgt->subsystem_ids, sid);		/* [한국어] sid 비트 set — 다른 create가 같은 id를 못 받게 */
	RB_INSERT(subsystem_tree, &tgt->subsystems, subsystem);	/* [한국어] tgt의 NQN-keyed RB tree에 삽입 (find subsystem O(log N)) */

	SPDK_DTRACE_PROBE1(nvmf_subsystem_create, subsystem->subnqn);	/* [한국어] USDT trace point — DTrace/perf로 관측 가능 */

	return subsystem;					/* [한국어] 호출자에게 새 subsystem 핸들 반환 (state=INACTIVE) */
}

/*
 * [한국어]
 * nvmf_host_free - 호스트 ACL 엔트리 메모리 해제
 *
 * @host: 해제할 호스트 객체 (nvmf_subsystem_find_host로 얻은 포인터)
 *
 * DH-HMAC-CHAP 인증 키(dhchap_key / dhchap_ctrlr_key)가 설정된 경우
 * keyring의 ref count를 감소시킨 후 host 구조체를 free한다.
 * TAILQ 제거 후 반드시 이 함수로 정리해야 키 참조 누수가 없다.
 *
 * 실행 컨텍스트: subsystem->mutex 보유 상태(remove_host), 또는 destroy 경로.
 *
 * 호출 체인:
 *   nvmf_subsystem_remove_host → [nvmf_host_free]
 *   spdk_nvmf_subsystem_destroy → nvmf_subsystem_remove_host → [nvmf_host_free]
 */
static void
nvmf_host_free(struct spdk_nvmf_host *host)
{
	spdk_keyring_put_key(host->dhchap_key);		/* [한국어] host 인증용 DH-HMAC-CHAP 키 참조 반환 (keyring ref -1) */
	spdk_keyring_put_key(host->dhchap_ctrlr_key);	/* [한국어] 컨트롤러 인증용 CHAP 키 참조 반환 (bidirectional auth 키) */
	free(host);					/* [한국어] 호스트 구조체 메모리 반환 */
}

/* Must hold subsystem->mutex while calling this function */
/*
 * [한국어]
 * nvmf_subsystem_remove_host - TAILQ에서 호스트 항목을 제거하고 메모리 해제
 *
 * @subsystem: 대상 subsystem (mutex 보유 상태에서 호출)
 * @host:      제거할 호스트 엔트리 (nvmf_subsystem_find_host로 이미 탐색 완료)
 *
 * 주의: 반드시 pthread_mutex_lock(&subsystem->mutex)를 보유한 상태에서 호출.
 * 호출자가 mutex 보유를 보장하므로 내부에서는 별도 락 없이 hosts 리스트를 수정한다.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_remove_host → [nvmf_subsystem_remove_host] → nvmf_host_free
 *   spdk_nvmf_subsystem_destroy → [nvmf_subsystem_remove_host] → nvmf_host_free
 */
static void
nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_host *host)
{
	TAILQ_REMOVE(&subsystem->hosts, host, link);	/* [한국어] subsystem의 hosts TAILQ에서 해당 엔트리 분리 */
	nvmf_host_free(host);				/* [한국어] 키 ref 반환 및 메모리 해제 */
}

/*
 * [한국어]
 * _nvmf_subsystem_remove_listener - listener를 subsystem에서 제거하는 내부 공통 구현
 *
 * @subsystem: 대상 subsystem
 * @listener:  제거할 listener 객체 (ana_state, opts 포함)
 * @stop:      true이면 transport에 listen 중단 요청(stop_listen), false이면 단순 해제
 *
 * spdk_nvmf_subsystem_remove_listener(stop=false)와
 * nvmf_subsystem_remove_all_listeners(stop=true, destroy 경로) 양쪽에서 공유.
 * 다음을 수행한다:
 *   1) stop=true이면 transport에 stop_listen 호출 (신규 연결 차단)
 *   2) 이 listener를 사용하는 모든 ctrlr의 ctrlr->listener를 NULL로 설정
 *   3) TAILQ에서 listener 제거
 *   4) discovery subsystem이면 mDNS PRR 업데이트
 *   5) discovery log notice 발송 (호스트 재조회 유도)
 *   6) ana_state/sock_impl/listener 메모리 해제, listener_id 비트맵 clear
 *
 * 실행 컨텍스트: subsystem->thread (INACTIVE 또는 PAUSED 상태에서 호출).
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_remove_listener(stop=false) → [_nvmf_subsystem_remove_listener]
 *   nvmf_subsystem_remove_all_listeners(stop=true) → [_nvmf_subsystem_remove_listener]
 */
static void
_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
				struct spdk_nvmf_subsystem_listener *listener,
				bool stop)
{
	struct spdk_nvmf_transport *transport;	/* [한국어] listener의 transport 핸들 (stop_listen 호출용) */
	struct spdk_nvmf_ctrlr *ctrlr;		/* [한국어] 현재 subsystem에 연결된 ctrlr 순회용 */

	if (stop) {							/* [한국어] destroy 경로: transport에 listen 중단 요청 */
		assert(nvmf_subsystem_listener_is_active(listener));		/* [한국어] active 아닌 listener를 stop하면 안 됨 */

		transport = spdk_nvmf_tgt_get_transport(subsystem->tgt, listener->trid->trstring);
		/* [한국어] trid의 transport 이름(trstring)으로 transport 객체 조회 */
		if (transport != NULL) {
			spdk_nvmf_transport_stop_listen(transport, listener->trid);
			/* [한국어] transport에게 이 trid에서 신규 accept 중단을 요청 */
		}
	}

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {	/* [한국어] 이 listener를 사용 중인 ctrlr 찾기 */
		if (ctrlr->listener == listener) {
			ctrlr->listener = NULL;			/* [한국어] ctrlr의 listener 역참조 무효화 (dangling pointer 방지) */
		}
	}

	TAILQ_REMOVE(&subsystem->listeners, listener, link);	/* [한국어] subsystem의 listeners TAILQ에서 제거 */
	if (spdk_nvmf_subsystem_is_discovery(listener->subsystem)) {
		nvmf_tgt_update_mdns_prr(listener->subsystem->tgt);
		/* [한국어] discovery subsystem의 listener가 제거되면 mDNS PRR(Pull Request Referral) 업데이트 */
	}
	spdk_nvmf_send_discovery_log_notice(listener->subsystem->tgt, NULL);
	/* [한국어] 호스트에게 discovery log가 변경되었음을 AER(Async Event)로 통지 */
	free(listener->ana_state);				/* [한국어] ANA 상태 배열(max_nsid 크기) 해제 */
	spdk_bit_array_clear(subsystem->used_listener_ids, listener->id);
	/* [한국어] 이 listener의 id 비트 clear — 이후 재사용 가능하게 */
	free(listener->opts.sock_impl);				/* [한국어] sock_impl 이름 문자열 해제 (strdup으로 복사된 경우) */
	free(listener);						/* [한국어] listener 구조체 자체 해제 */
}

/*
 * [한국어]
 * _nvmf_subsystem_destroy_msg - spdk_thread_send_msg를 통한 _nvmf_subsystem_destroy 호출 래퍼
 *
 * @cb_arg: struct spdk_nvmf_subsystem* (subsystem 포인터를 void*로 전달)
 *
 * 아직 활성 ctrlr가 남아있을 때 destroy를 재시도하기 위해
 * spdk_thread_send_msg로 subsystem->thread에 메시지를 보낼 때 사용하는 콜백.
 * ctrlr disconnect가 비동기이므로 반복 체크 방식으로 완료를 기다린다.
 *
 * 호출 체인:
 *   _nvmf_subsystem_destroy (ctrlr 잔류) → spdk_thread_send_msg → [_nvmf_subsystem_destroy_msg]
 *     → _nvmf_subsystem_destroy
 */
static void
_nvmf_subsystem_destroy_msg(void *cb_arg)
{
	struct spdk_nvmf_subsystem *subsystem = cb_arg;	/* [한국어] void* 인자를 subsystem 포인터로 복원 */

	_nvmf_subsystem_destroy(subsystem);		/* [한국어] 잔류 ctrlr 재확인 후 실제 파괴 시도 */
}

/*
 * [한국어]
 * _nvmf_subsystem_destroy - subsystem 내부 파괴 실행 (ctrlr 완전 종료 후 호출)
 *
 * @subsystem: 파괴할 subsystem (destroying=true 플래그 설정된 상태)
 * @return: 0=완전 파괴 완료, -EINPROGRESS=아직 ctrlr가 남아 재시도 예약됨
 *
 * spdk_nvmf_subsystem_destroy에서 직접 호출되거나,
 * ctrlr가 남아있어 비동기 대기 후 _nvmf_subsystem_destroy_msg를 통해 재호출됨.
 * 모든 ctrlr가 gone이면:
 *   1) 모든 NS 제거 (bdev close / claim release)
 *   2) 대기 중인 state_change ctx 모두 ECANCELED로 취소
 *   3) NS 배열, ANA 그룹 배열 해제
 *   4) tgt의 RB tree에서 제거, subsystem_ids 비트맵 clear
 *   5) mutex 파괴, listener_ids 비트맵 해제
 *   6) subsystem 메모리 해제 후 async_destroy_cb 호출
 *
 * 실행 컨텍스트: subsystem->thread (메시지로 디스패치되므로 thread-safe).
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_destroy → [_nvmf_subsystem_destroy]
 *   _nvmf_subsystem_destroy_msg → [_nvmf_subsystem_destroy]
 */
static int
_nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem)
{
	struct nvmf_subsystem_state_change_ctx *ctx;		/* [한국어] 취소할 state_change 요청 항목 */
	struct spdk_nvmf_ns		*ns;			/* [한국어] 제거할 NS 순회용 */
	nvmf_subsystem_destroy_cb	async_destroy_cb = NULL;	/* [한국어] 파괴 완료 콜백 (free 전에 추출) */
	void				*async_destroy_cb_arg = NULL;	/* [한국어] 콜백 인자 */

	if (!TAILQ_EMPTY(&subsystem->ctrlrs)) {			/* [한국어] 아직 connect된 ctrlr가 있으면 파괴 연기 */
		SPDK_DEBUGLOG(nvmf, "subsystem %p %s has active controllers\n", subsystem, subsystem->subnqn);
		subsystem->async_destroy = true;		/* [한국어] 비동기 destroy 진행 중 플래그 — 완료 시 cb 호출용 */
		spdk_thread_send_msg(subsystem->thread, _nvmf_subsystem_destroy_msg, subsystem);
		/* [한국어] subsystem->thread에 메시지를 보내 다음 poll iteration에서 재시도 */
		return -EINPROGRESS;				/* [한국어] 아직 미완료 — 호출자는 이 코드를 무시해야 함 */
	}

	ns = spdk_nvmf_subsystem_get_first_ns(subsystem);	/* [한국어] 첫 번째 NS부터 순서대로 제거 */
	while (ns != NULL) {
		struct spdk_nvmf_ns *next_ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns);
		/* [한국어] remove_ns 호출 전에 next 포인터 저장 (ns 해제 후 접근 금지) */

		spdk_nvmf_subsystem_remove_ns(subsystem, ns->opts.nsid);
		/* [한국어] bdev 닫기, claim release, registrant 제거, transport ns_remove 통지 */
		ns = next_ns;
	}

	while ((ctx = TAILQ_FIRST(&subsystem->state_changes))) {	/* [한국어] 대기 중인 상태 전이 요청 모두 취소 */
		SPDK_WARNLOG("subsystem %s has pending state change requests\n", subsystem->subnqn);
		TAILQ_REMOVE(&subsystem->state_changes, ctx, link);	/* [한국어] 큐에서 제거 */
		if (ctx->cb_fn != NULL) {
			ctx->cb_fn(subsystem, ctx->cb_arg, -ECANCELED);	/* [한국어] 요청자에게 ECANCELED 통지 */
		}
		free(ctx);						/* [한국어] state_change ctx 메모리 해제 */
	}

	free(subsystem->ns);					/* [한국어] NS 포인터 배열 해제 (max_nsid 크기) */
	free(subsystem->ana_group);				/* [한국어] ANA 그룹 카운터 배열 해제 */

	RB_REMOVE(subsystem_tree, &subsystem->tgt->subsystems, subsystem);
	/* [한국어] tgt의 NQN-keyed RB tree에서 이 subsystem 제거 — 이후 find_subsystem 불가 */
	assert(spdk_bit_array_get(subsystem->tgt->subsystem_ids, subsystem->id) == true);
	/* [한국어] 반드시 이 id 비트가 set되어 있어야 함 (일관성 검증) */
	spdk_bit_array_clear(subsystem->tgt->subsystem_ids, subsystem->id);
	/* [한국어] id 비트 clear — 다른 subsystem이 이 id를 재사용 가능 */

	pthread_mutex_destroy(&subsystem->mutex);		/* [한국어] hosts/state_changes 보호 mutex 파괴 */

	spdk_bit_array_free(&subsystem->used_listener_ids);	/* [한국어] listener id 비트맵 해제 */

	if (subsystem->async_destroy) {				/* [한국어] 비동기 경로였다면 콜백 포인터 먼저 추출 */
		async_destroy_cb = subsystem->async_destroy_cb;
		async_destroy_cb_arg = subsystem->async_destroy_cb_arg;
	}

	free(subsystem);					/* [한국어] subsystem 구조체 최종 해제 — 이후 subsystem 포인터 사용 금지 */

	if (async_destroy_cb) {
		async_destroy_cb(async_destroy_cb_arg);		/* [한국어] 파괴 완료를 호출자(RPC 등)에게 통지 */
	}

	return 0;
}

/*
 * [한국어]
 * _nvmf_subsystem_get_first_zoned_ns - subsystem의 첫 번째 ZNS namespace 반환
 *
 * @subsystem: 탐색할 subsystem
 * @return: ZNS CSI(Command Set Identifier)를 가진 첫 번째 NS, 없으면 NULL
 *
 * NVMe ZNS(Zoned Namespace Storage) TP 4053에서 정의된 csi==SPDK_NVME_CSI_ZNS인
 * NS를 찾는다. add_ns_ext에서 ZNS 일관성 검증(zone_append 지원/크기 동일 여부)에
 * 사용된다 — 첫 번째 ZNS NS와 새로 추가하려는 ZNS NS가 같은 설정을 가져야 한다.
 *
 * 실행 컨텍스트: spdk_nvmf_subsystem_add_ns_ext 내부(INACTIVE/PAUSED 상태).
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_add_ns_ext → [_nvmf_subsystem_get_first_zoned_ns]
 */
static struct spdk_nvmf_ns *
_nvmf_subsystem_get_first_zoned_ns(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_ns *ns = spdk_nvmf_subsystem_get_first_ns(subsystem);
	/* [한국어] 첫 번째 NS부터 순차 탐색 시작 */
	while (ns != NULL) {
		if (ns->csi == SPDK_NVME_CSI_ZNS) {		/* [한국어] ZNS CSI(0x02)를 가진 NS를 찾으면 즉시 반환 */
			return ns;
		}
		ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns);	/* [한국어] 다음 NS로 이동 */
	}
	return NULL;						/* [한국어] ZNS NS 없음 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_destroy - subsystem 비동기 파괴 시작
 *
 * @subsystem: 파괴할 subsystem (반드시 INACTIVE 상태여야 함)
 * @cpl_cb:    파괴 완료 시 호출될 콜백 (NULL 가능)
 * @cpl_cb_arg: 콜백 인자
 * @return: 0=비동기 시작 또는 즉시 완료, -EINVAL/-EAGAIN/-EALREADY 등 오류
 *
 * INACTIVE 상태가 아니면 거부 (호출자가 먼저 stop을 호출해야 한다).
 * 본 함수는 listener/host를 모두 제거하고 모든 NS도 제거한 뒤
 * _nvmf_subsystem_destroy를 호출한다. NS 제거가 비동기일 수 있으므로
 * destroy 자체도 비동기로 완료된다 (cpl_cb로 통지).
 *
 * 실행 컨텍스트: subsystem->thread에서 호출 필수 (assert로 강제).
 *
 * 호출 체인:
 *   RPC remove → spdk_nvmf_subsystem_stop → [spdk_nvmf_subsystem_destroy]
 *     → nvmf_subsystem_remove_all_listeners → nvmf_subsystem_remove_host (각 host)
 *     → _nvmf_subsystem_destroy → free
 */
int
spdk_nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem, nvmf_subsystem_destroy_cb cpl_cb,
			    void *cpl_cb_arg)
{
	struct spdk_nvmf_host *host, *host_tmp;
	struct spdk_nvmf_transport *transport;

	if (!subsystem) {
		return -EINVAL;
	}

	SPDK_DTRACE_PROBE1(nvmf_subsystem_destroy, subsystem->subnqn);

	assert(spdk_get_thread() == subsystem->thread);

	if (subsystem->state != SPDK_NVMF_SUBSYSTEM_INACTIVE) {
		SPDK_ERRLOG("Subsystem can only be destroyed in inactive state, %s state %d\n",
			    subsystem->subnqn, subsystem->state);
		return -EAGAIN;
	}
	if (subsystem->destroying) {
		SPDK_ERRLOG("Subsystem destruction is already started\n");
		assert(0);
		return -EALREADY;
	}

	subsystem->destroying = true;

	SPDK_DEBUGLOG(nvmf, "subsystem is %p %s\n", subsystem, subsystem->subnqn);

	nvmf_subsystem_remove_all_listeners(subsystem, false);

	pthread_mutex_lock(&subsystem->mutex);

	TAILQ_FOREACH_SAFE(host, &subsystem->hosts, link, host_tmp) {
		for (transport = spdk_nvmf_transport_get_first(subsystem->tgt); transport;
		     transport = spdk_nvmf_transport_get_next(transport)) {
			if (transport->ops->subsystem_remove_host) {
				transport->ops->subsystem_remove_host(transport, subsystem, host->nqn);
			}
		}
		nvmf_subsystem_remove_host(subsystem, host);
	}

	pthread_mutex_unlock(&subsystem->mutex);

	subsystem->async_destroy_cb = cpl_cb;
	subsystem->async_destroy_cb_arg = cpl_cb_arg;

	return _nvmf_subsystem_destroy(subsystem);
}

/* we have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_nvmf_subsystem_state spdk_nvmf_subsystem_state_t;
/* [한국어] astyle 포매터가 함수 선언 내 enum 타입을 처리하지 못하는 문제 우회용 typedef */

/*
 * [한국어]
 * nvmf_subsystem_get_intermediate_state - 상태 전이 중간 상태 계산
 *
 * @current_state:   현재 subsystem 상태
 * @requested_state: 목표(최종) 상태
 * @return: 중간 전이 상태 (예: INACTIVE→ACTIVE이면 먼저 ACTIVATING 거침)
 *
 * NVMe-oF subsystem 상태 머신에서 두 상태 사이에는 반드시 중간 상태가 있다:
 *   current → INACTIVE:  DEACTIVATING 경유
 *   current → ACTIVE:    current==PAUSED이면 RESUMING, 그외 ACTIVATING 경유
 *   current → PAUSED:    PAUSING 경유
 * 중간 상태는 poll group 순회 중 상태를 표시하고, 완료 시 최종 상태로 전이한다.
 * NUM_STATES 반환 시 버그(assert false) 경로이며 호출자가 assert로 검증한다.
 *
 * 호출 체인:
 *   nvmf_subsystem_do_state_change → [nvmf_subsystem_get_intermediate_state]
 *   subsystem_state_change_done → [nvmf_subsystem_get_intermediate_state]
 */
static spdk_nvmf_subsystem_state_t
nvmf_subsystem_get_intermediate_state(enum spdk_nvmf_subsystem_state current_state,
				      enum spdk_nvmf_subsystem_state requested_state)
{
	switch (requested_state) {
	case SPDK_NVMF_SUBSYSTEM_INACTIVE:			/* [한국어] stop 요청: DEACTIVATING 경유 후 INACTIVE */
		return SPDK_NVMF_SUBSYSTEM_DEACTIVATING;
	case SPDK_NVMF_SUBSYSTEM_ACTIVE:
		if (current_state == SPDK_NVMF_SUBSYSTEM_PAUSED) {	/* [한국어] resume: PAUSED → RESUMING → ACTIVE */
			return SPDK_NVMF_SUBSYSTEM_RESUMING;
		} else {					/* [한국어] start: INACTIVE → ACTIVATING → ACTIVE */
			return SPDK_NVMF_SUBSYSTEM_ACTIVATING;
		}
	case SPDK_NVMF_SUBSYSTEM_PAUSED:			/* [한국어] pause: ACTIVE → PAUSING → PAUSED */
		return SPDK_NVMF_SUBSYSTEM_PAUSING;
	default:
		assert(false);					/* [한국어] 불가능한 목표 상태 — 버그 */
		return SPDK_NVMF_SUBSYSTEM_NUM_STATES;		/* [한국어] 호출자 assert를 트리거하는 sentinel 값 */
	}
}

/*
 * [한국어]
 * nvmf_subsystem_set_state - atomic CAS로 subsystem 상태 전이
 *
 * @subsystem: 대상 subsystem
 * @state:     목표 상태
 * @return: 0=성공 (실제 직전 상태가 예상과 일치), 음수=오류
 *
 * subsystem 상태 머신은 다음 그래프를 따른다:
 *   INACTIVE → ACTIVATING → ACTIVE → PAUSING → PAUSED → RESUMING → ACTIVE
 *                                  ↓                                 ↓
 *                            DEACTIVATING ← ─────────────────────────┘
 *                                  ↓
 *                              INACTIVE
 * 각 전이는 직전 상태가 정확히 expected_old_state여야 하며, 본 함수는
 * GCC __atomic_compare_exchange_n으로 lockless 전이를 시도한다. 실패 시
 * (전이 실패/중단된 ACTIVATING/RESUMING의 롤백 케이스 등) 두 번째 CAS로
 * 원래 상태로 복원한다.
 *
 * 동기화: state는 atomic이며 mutex 없이 접근 — multi-thread에서 동시 호출
 * 가능성을 가정한다 (단, 실제 호출 위치는 subsystem->thread에 직렬화됨).
 */
static int
nvmf_subsystem_set_state(struct spdk_nvmf_subsystem *subsystem,
			 enum spdk_nvmf_subsystem_state state)
{
	enum spdk_nvmf_subsystem_state actual_old_state, expected_old_state;	/* [한국어] CAS 비교용 / 실제 직전 상태 */
	bool exchanged;								/* [한국어] CAS 성공 여부 */

	switch (state) {							/* [한국어] 목표 상태로부터 정당한 직전 상태를 도출 (state machine) */
	case SPDK_NVMF_SUBSYSTEM_INACTIVE:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_DEACTIVATING;		/* [한국어] DEACTIVATING → INACTIVE */
		break;
	case SPDK_NVMF_SUBSYSTEM_ACTIVATING:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_INACTIVE;		/* [한국어] INACTIVE → ACTIVATING (start 시작) */
		break;
	case SPDK_NVMF_SUBSYSTEM_ACTIVE:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_ACTIVATING;		/* [한국어] ACTIVATING → ACTIVE (start 완료) */
		break;
	case SPDK_NVMF_SUBSYSTEM_PAUSING:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_ACTIVE;		/* [한국어] ACTIVE → PAUSING */
		break;
	case SPDK_NVMF_SUBSYSTEM_PAUSED:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_PAUSING;		/* [한국어] PAUSING → PAUSED */
		break;
	case SPDK_NVMF_SUBSYSTEM_RESUMING:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_PAUSED;		/* [한국어] PAUSED → RESUMING */
		break;
	case SPDK_NVMF_SUBSYSTEM_DEACTIVATING:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_ACTIVE;		/* [한국어] ACTIVE → DEACTIVATING (stop 시작) */
		break;
	default:
		assert(false);							/* [한국어] 그 외 상태로의 직접 전이는 금지 */
		return -1;
	}

	actual_old_state = expected_old_state;					/* [한국어] CAS는 실제 값을 actual_old_state에 기록한다 */
	exchanged = __atomic_compare_exchange_n(&subsystem->state, &actual_old_state, state, false,
						__ATOMIC_RELAXED, __ATOMIC_RELAXED);
	/* [한국어] GCC built-in atomic CAS: state == actual_old_state면 state=새 state, 실패면 actual_old_state=현재값.
	 * memory order RELAXED — 상태 전이만 동기화하고 자료구조 가시성은 별도 책임. */
	if (spdk_unlikely(exchanged == false)) {				/* [한국어] CAS 실패 — 롤백 케이스 처리 (아래 if 체인) */
		if (actual_old_state == SPDK_NVMF_SUBSYSTEM_RESUMING &&
		    state == SPDK_NVMF_SUBSYSTEM_ACTIVE) {
			expected_old_state = SPDK_NVMF_SUBSYSTEM_RESUMING;
		}
		/* This is for the case when activating the subsystem fails. */
		if (actual_old_state == SPDK_NVMF_SUBSYSTEM_ACTIVATING &&
		    state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING) {
			expected_old_state = SPDK_NVMF_SUBSYSTEM_ACTIVATING;
		}
		/* This is for the case when resuming the subsystem fails. */
		if (actual_old_state == SPDK_NVMF_SUBSYSTEM_RESUMING &&
		    state == SPDK_NVMF_SUBSYSTEM_PAUSING) {
			expected_old_state = SPDK_NVMF_SUBSYSTEM_RESUMING;
		}
		/* This is for the case when stopping paused subsystem */
		if (actual_old_state == SPDK_NVMF_SUBSYSTEM_PAUSED &&
		    state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING) {
			expected_old_state = SPDK_NVMF_SUBSYSTEM_PAUSED;
		}
		actual_old_state = expected_old_state;
		__atomic_compare_exchange_n(&subsystem->state, &actual_old_state, state, false,
					    __ATOMIC_RELAXED, __ATOMIC_RELAXED);
	}
	assert(actual_old_state == expected_old_state);
	return actual_old_state - expected_old_state;
}

static void nvmf_subsystem_do_state_change(struct nvmf_subsystem_state_change_ctx *ctx);
/* [한국어] forward decl: 상태 전이 실행 함수. 큐에 새 요청이 있을 때 다음 요청을 시작하는 데 사용. */

/*
 * [한국어]
 * _nvmf_subsystem_state_change_complete - 상태 전이 완료 처리 (ctx->thread에서 실행)
 *
 * @_ctx: nvmf_subsystem_state_change_ctx* (spdk_thread_exec_msg 콜백용 void*)
 *
 * 현재 ctx를 state_changes TAILQ에서 제거하고 cb_fn으로 결과를 통지한다.
 * 큐에 다음 요청이 있으면 nvmf_subsystem_do_state_change를 호출하여
 * 연속 상태 전이를 직렬로 처리한다(예: pause → stop).
 *
 * 실행 컨텍스트: ctx->thread (spdk_thread_exec_msg로 디스패치됨).
 * 동기화: TAILQ 수정 시 subsystem->mutex 보유.
 *
 * 호출 체인:
 *   nvmf_subsystem_state_change_complete → spdk_thread_exec_msg → [_nvmf_subsystem_state_change_complete]
 *     → ctx->cb_fn → nvmf_subsystem_do_state_change (next)
 */
static void
_nvmf_subsystem_state_change_complete(void *_ctx)
{
	struct nvmf_subsystem_state_change_ctx *next, *ctx = _ctx;	/* [한국어] 현재 완료된 ctx 및 다음 pending ctx */
	struct spdk_nvmf_subsystem *subsystem = ctx->subsystem;		/* [한국어] 역참조 subsystem */

	pthread_mutex_lock(&subsystem->mutex);				/* [한국어] state_changes TAILQ 수정 구간 보호 */
	assert(TAILQ_FIRST(&subsystem->state_changes) == ctx);		/* [한국어] 항상 HEAD가 현재 처리 중이어야 함 */
	TAILQ_REMOVE(&subsystem->state_changes, ctx, link);		/* [한국어] 완료된 ctx를 큐에서 제거 */
	next = TAILQ_FIRST(&subsystem->state_changes);			/* [한국어] 다음 대기 요청 확인 */
	pthread_mutex_unlock(&subsystem->mutex);			/* [한국어] mutex 해제 */

	if (ctx->cb_fn != NULL) {
		ctx->cb_fn(subsystem, ctx->cb_arg, ctx->status);	/* [한국어] 요청자에게 완료 통지 (status 0=성공) */
	}
	free(ctx);							/* [한국어] 완료된 ctx 해제 */

	if (next != NULL) {
		nvmf_subsystem_do_state_change(next);			/* [한국어] 큐에 대기 중인 다음 상태 전이 즉시 시작 */
	}
}

/*
 * [한국어]
 * nvmf_subsystem_state_change_complete - 상태 전이 완료 결과를 ctx->thread에 전달
 *
 * @ctx:    완료된 상태 전이 요청 ctx
 * @status: 0=성공, 음수=오류 코드
 *
 * poll group 순회 완료 콜백(subsystem_state_change_done)에서 호출되며,
 * ctx->thread(요청자 thread)로 _nvmf_subsystem_state_change_complete를
 * 비동기로 디스패치하여 콜백/큐 처리를 요청자 컨텍스트에서 안전하게 수행한다.
 *
 * 실행 컨텍스트: 임의의 poll group thread (spdk_for_each_channel 완료 콜백).
 *
 * 호출 체인:
 *   subsystem_state_change_done → [nvmf_subsystem_state_change_complete]
 *     → spdk_thread_exec_msg → _nvmf_subsystem_state_change_complete
 */
static void
nvmf_subsystem_state_change_complete(struct nvmf_subsystem_state_change_ctx *ctx, int status)
{
	ctx->status = status;				/* [한국어] 완료 결과를 ctx에 저장 (exec_msg 콜백이 읽음) */
	spdk_thread_exec_msg(ctx->thread, _nvmf_subsystem_state_change_complete, ctx);
	/* [한국어] ctx->thread에서 _complete 함수 실행 — cross-thread safe 콜백 디스패치 */
}

/*
 * [한국어]
 * subsystem_state_change_revert_done - 실패 후 원래 상태로 롤백 완료 콜백
 *
 * @i:      spdk_for_each_channel 이터레이터 (ctx 담고 있음)
 * @status: 롤백 for_each_channel 결과 (무시됨 — 이미 오류 경로)
 *
 * 상태 전이가 실패했을 때 모든 poll group을 원래 상태로 되돌리는
 * spdk_for_each_channel이 완료되면 이 함수가 호출된다.
 * 롤백 자체가 실패해도 추가 복구 수단이 없으므로 ERRLOG만 남기고 진행.
 * 요청자에게 -1(실패)로 완료 통지.
 *
 * 실행 컨텍스트: 마지막 poll group thread → 즉시 ctx->thread로 exec_msg 디스패치.
 *
 * 호출 체인:
 *   subsystem_state_change_done (실패) → spdk_for_each_channel → [subsystem_state_change_revert_done]
 *     → nvmf_subsystem_state_change_complete (status=-1)
 */
static void
subsystem_state_change_revert_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_subsystem_state_change_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 이터레이터에서 state_change ctx 복원 */

	/* Nothing to be done here if the state setting fails, we are just screwed. */
	if (nvmf_subsystem_set_state(ctx->subsystem, ctx->requested_state)) {
		/* [한국어] 원래 상태(requested_state = original_state로 교체됨)로 CAS 시도 — 실패 시 상태 불일치 */
		SPDK_ERRLOG("Unable to revert the subsystem state after operation failure.\n");
	}

	/* return a failure here. This function only exists in an error path. */
	nvmf_subsystem_state_change_complete(ctx, -1);		/* [한국어] 상태 전이 실패를 요청자에게 통지 */
}

/*
 * [한국어]
 * subsystem_state_change_done - 모든 poll group 상태 변경 완료 후 최종 상태 확정
 *
 * @i:      spdk_for_each_channel 이터레이터
 * @status: 0=모든 poll group 성공, 비0=어느 poll group에서 실패
 *
 * spdk_for_each_channel(subsystem_state_change_on_pg)이 모든 poll group을 순회한 후
 * 이 함수가 최종 완료 콜백으로 호출된다.
 * 성공 시 subsystem 상태를 requested_state로 원자적으로 확정(CAS).
 * 실패 시 원래 상태(original_state)로 롤백하는 for_each_channel을 다시 시작한다:
 *   - 롤백용 중간 상태로 CAS 시도 → 실패 시 즉시 complete(-1)
 *   - 성공 시 requested_state = original_state로 교체 후 for_each_channel 재시작
 *   - 롤백 완료 시 subsystem_state_change_revert_done → complete(-1)
 *
 * 실행 컨텍스트: 마지막 poll group thread의 완료 콜백.
 *
 * 호출 체인:
 *   spdk_for_each_channel(subsystem_state_change_on_pg) → [subsystem_state_change_done]
 *     → nvmf_subsystem_set_state → nvmf_subsystem_state_change_complete
 */
static void
subsystem_state_change_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_subsystem_state_change_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 이터레이터에서 state_change ctx 복원 */
	enum spdk_nvmf_subsystem_state intermediate_state;	/* [한국어] 롤백 시 거칠 중간 상태 */

	SPDK_DTRACE_PROBE4(nvmf_subsystem_change_state_done, ctx->subsystem->subnqn,
			   ctx->requested_state, ctx->original_state, status);
	/* [한국어] USDT trace: 전이 완료 (NQN, requested, original, status 기록) */

	if (status == 0) {					/* [한국어] 모든 poll group 성공 → 최종 상태 확정 시도 */
		status = nvmf_subsystem_set_state(ctx->subsystem, ctx->requested_state);
		/* [한국어] 중간 상태(ACTIVATING 등) → 최종 상태(ACTIVE 등) CAS */
		if (status) {
			status = -1;				/* [한국어] CAS 실패를 통일된 오류 코드로 정규화 */
		}
	}

	if (status) {						/* [한국어] 실패 경로: 원래 상태로 롤백 시작 */
		intermediate_state = nvmf_subsystem_get_intermediate_state(ctx->requested_state,
				     ctx->original_state);
		/* [한국어] 롤백 방향: requested_state→original_state의 중간 상태 계산 */
		assert(intermediate_state != SPDK_NVMF_SUBSYSTEM_NUM_STATES);
		/* [한국어] 유효하지 않은 상태 전이 방향이면 버그 */

		if (nvmf_subsystem_set_state(ctx->subsystem, intermediate_state)) {
			goto out;				/* [한국어] 롤백용 중간 상태 CAS 실패 → 직접 complete(-1) */
		}
		ctx->requested_state = ctx->original_state;	/* [한국어] 롤백 목표를 original_state로 교체 */
		spdk_for_each_channel(ctx->subsystem->tgt,
				      subsystem_state_change_on_pg,
				      ctx,
				      subsystem_state_change_revert_done);
		/* [한국어] 모든 poll group에 롤백 상태 적용 후 revert_done 호출 */
		return;
	}

out:
	nvmf_subsystem_state_change_complete(ctx, status);	/* [한국어] 최종 완료(성공 또는 복구불능 실패) 통지 */
}

/*
 * [한국어]
 * subsystem_state_change_continue - 각 poll group 상태 변경 완료 후 for_each_channel 계속
 *
 * @ctx:    spdk_io_channel_iter* (subsystem_state_change_on_pg에서 전달)
 * @status: 해당 poll group 작업 결과 (0=성공, 비0=실패)
 *
 * nvmf_poll_group_add/remove/pause/resume_subsystem이 완료될 때 호출되는 콜백.
 * DTRACE로 현재 poll group thread/NQN/상태를 기록한 후
 * spdk_for_each_channel_continue로 다음 poll group으로 순회를 이어간다.
 * status가 비0이면 for_each_channel이 즉시 중단되고 done 콜백이 호출된다.
 *
 * 실행 컨텍스트: 해당 poll group의 스레드 (각 채널마다 해당 스레드에서 실행).
 *
 * 호출 체인:
 *   nvmf_poll_group_add/remove/pause/resume_subsystem 완료
 *     → [subsystem_state_change_continue] → spdk_for_each_channel_continue
 */
static void
subsystem_state_change_continue(void *ctx, int status)
{
	struct spdk_io_channel_iter *i = ctx;			/* [한국어] void* 인자를 이터레이터로 복원 */
	struct nvmf_subsystem_state_change_ctx *_ctx __attribute__((unused));
	/* [한국어] DTRACE 로깅 전용 — 컴파일러 unused 경고 억제 */

	_ctx = spdk_io_channel_iter_get_ctx(i);			/* [한국어] 이터레이터에서 ctx 추출 (DTRACE 전용) */
	SPDK_DTRACE_PROBE3(nvmf_pg_change_state_done, _ctx->subsystem->subnqn,
			   _ctx->requested_state, spdk_thread_get_id(spdk_get_thread()));
	/* [한국어] USDT trace: poll group 상태 변경 완료 (NQN, 목표상태, thread id) */

	spdk_for_each_channel_continue(i, status);		/* [한국어] 다음 poll group으로 순회 계속 (status!=0이면 중단) */
}

/*
 * [한국어]
 * subsystem_state_change_on_pg - 각 poll group에서 subsystem 상태 변경 적용
 *
 * @i: spdk_for_each_channel 이터레이터 (ctx, 현재 io_channel 담고 있음)
 *
 * spdk_for_each_channel로 모든 poll group을 순회하면서 각 poll group에
 * subsystem의 상태 전이를 적용하는 핵심 콜백이다.
 * 목표 상태(requested_state)에 따라:
 *   INACTIVE: poll group에서 subsystem 제거 (qpair disconnect 포함)
 *   ACTIVE:   ACTIVATING이면 poll group에 subsystem 추가,
 *             RESUMING이면 poll group의 subsystem I/O 재개
 *   PAUSED:   nsid 지정된 경우 해당 NS I/O만, 0이면 전체 I/O 중단
 * 각 작업은 비동기이며 subsystem_state_change_continue 콜백으로 완료를 알린다.
 *
 * 실행 컨텍스트: 각 poll group thread (for_each_channel이 thread-per-channel로 디스패치).
 * 이 함수는 재진입 안전하지 않으므로 동일 poll group에서 중복 호출 금지.
 *
 * 호출 체인:
 *   spdk_for_each_channel → [subsystem_state_change_on_pg]
 *     → nvmf_poll_group_add/remove/pause/resume_subsystem
 *     → subsystem_state_change_continue → spdk_for_each_channel_continue
 */
static void
subsystem_state_change_on_pg(struct spdk_io_channel_iter *i)
{
	struct nvmf_subsystem_state_change_ctx *ctx;	/* [한국어] 상태 전이 요청 컨텍스트 */
	struct spdk_io_channel *ch;			/* [한국어] 현재 poll group에 대응하는 io_channel */
	struct spdk_nvmf_poll_group *group;		/* [한국어] io_channel 컨텍스트로서의 poll_group */

	ctx = spdk_io_channel_iter_get_ctx(i);		/* [한국어] for_each_channel에 등록된 ctx 추출 */
	ch = spdk_io_channel_iter_get_channel(i);	/* [한국어] 현재 순회 중인 채널 (poll group 1:1 대응) */
	group = spdk_io_channel_get_ctx(ch);		/* [한국어] io_channel 컨텍스트 → spdk_nvmf_poll_group */

	SPDK_DTRACE_PROBE3(nvmf_pg_change_state, ctx->subsystem->subnqn,
			   ctx->requested_state, spdk_thread_get_id(spdk_get_thread()));
	/* [한국어] USDT trace: 이 poll group에서 상태 변경 시작 */
	switch (ctx->requested_state) {
	case SPDK_NVMF_SUBSYSTEM_INACTIVE:			/* [한국어] stop: 이 poll group에서 subsystem 제거 */
		nvmf_poll_group_remove_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		break;
	case SPDK_NVMF_SUBSYSTEM_ACTIVE:
		if (ctx->subsystem->state == SPDK_NVMF_SUBSYSTEM_ACTIVATING) {
			/* [한국어] start: ACTIVATING 중간 상태 → poll group에 subsystem 등록 */
			nvmf_poll_group_add_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		} else if (ctx->subsystem->state == SPDK_NVMF_SUBSYSTEM_RESUMING) {
			/* [한국어] resume: RESUMING 중간 상태 → poll group I/O 재개 */
			nvmf_poll_group_resume_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		}
		break;
	case SPDK_NVMF_SUBSYSTEM_PAUSED:			/* [한국어] pause: nsid 지정 NS 또는 전체 I/O 일시 중단 */
		nvmf_poll_group_pause_subsystem(group, ctx->subsystem, ctx->nsid, subsystem_state_change_continue,
						i);
		break;
	default:
		assert(false);					/* [한국어] 직접 전이 불가한 상태 — 버그 */
		break;
	}
}

/*
 * [한국어]
 * nvmf_subsystem_do_state_change - 실제 상태 전이 실행 (중간 상태 설정 → poll group 순회)
 *
 * @ctx: 상태 전이 요청 컨텍스트 (subsystem, requested_state, nsid, cb_fn 포함)
 *
 * 상태 전이의 실제 실행 진입점. 다음을 수행한다:
 *   1) 이미 requested_state면 즉시 성공 완료 (no-op)
 *   2) 중간 상태(ACTIVATING/PAUSING/DEACTIVATING 등) 계산
 *   3) CAS로 중간 상태로 전이 (실패 시 -1 반환)
 *   4) spdk_for_each_channel로 모든 poll group에 상태 변경 적용 시작
 *      → subsystem_state_change_done으로 최종 상태 확정
 *
 * 실행 컨텍스트: 큐에 새 요청이 삽입될 때 또는 _complete에서 next 처리.
 * 동기화: state는 atomic CAS, 큐 자체는 mutex 보호.
 *
 * 호출 체인:
 *   nvmf_subsystem_state_change → [nvmf_subsystem_do_state_change]
 *   _nvmf_subsystem_state_change_complete (next) → [nvmf_subsystem_do_state_change]
 *     → nvmf_subsystem_set_state → spdk_for_each_channel
 */
static void
nvmf_subsystem_do_state_change(struct nvmf_subsystem_state_change_ctx *ctx)
{
	struct spdk_nvmf_subsystem *subsystem = ctx->subsystem;		/* [한국어] 역참조 subsystem */
	enum spdk_nvmf_subsystem_state intermediate_state;		/* [한국어] 진입할 중간 상태 */
	int rc;								/* [한국어] CAS 결과 */

	SPDK_DTRACE_PROBE3(nvmf_subsystem_change_state, subsystem->subnqn,
			   ctx->requested_state, subsystem->state);
	/* [한국어] USDT trace: 상태 전이 시작 (NQN, 목표상태, 현재상태) */

	/* If we are already in the requested state, just call the callback immediately. */
	if (subsystem->state == ctx->requested_state) {		/* [한국어] 이미 목표 상태 — 즉시 성공 완료 */
		nvmf_subsystem_state_change_complete(ctx, 0);
		return;
	}

	intermediate_state = nvmf_subsystem_get_intermediate_state(subsystem->state,
			     ctx->requested_state);
	/* [한국어] 현재 상태 → 목표 상태 사이의 중간 상태 결정 */
	assert(intermediate_state != SPDK_NVMF_SUBSYSTEM_NUM_STATES);
	/* [한국어] 유효하지 않은 전이 방향은 버그 */

	ctx->original_state = subsystem->state;			/* [한국어] 롤백 시 원상복구용으로 현재 상태 저장 */
	rc = nvmf_subsystem_set_state(subsystem, intermediate_state);
	/* [한국어] 중간 상태로 atomic CAS — 이후 poll group들에게 이 중간 상태가 보임 */
	if (rc) {
		nvmf_subsystem_state_change_complete(ctx, -1);		/* [한국어] CAS 실패 — 즉시 오류 완료 */
		return;
	}

	spdk_for_each_channel(subsystem->tgt,
			      subsystem_state_change_on_pg,
			      ctx,
			      subsystem_state_change_done);
	/* [한국어] tgt의 모든 poll group을 순서대로 순회하며 상태 변경 적용
	 * (각 poll group thread에서 subsystem_state_change_on_pg 호출)
	 * 완료 시 subsystem_state_change_done으로 최종 상태 확정 */
}


/*
 * [한국어]
 * nvmf_subsystem_state_change - 상태 전이 요청을 직렬 큐에 삽입하고 필요 시 즉시 시작
 *
 * @subsystem:       대상 subsystem
 * @nsid:            pause 대상 NSID (0=전체, >0=특정 NS만, start/stop에서는 0)
 * @requested_state: 요청하는 최종 상태 (ACTIVE/INACTIVE/PAUSED)
 * @cb_fn:           완료 콜백 (NULL 가능)
 * @cb_arg:          콜백 인자
 * @return: 0=큐 삽입 성공(비동기), -EINVAL=SPDK thread 외부 호출, -ENOMEM=ctx 할당 실패
 *
 * 상태 전이는 직렬화가 필요하므로(예: pause 완료 전 stop 요청 가능)
 * TAILQ에 순서대로 삽입하고, 큐가 비어있으면 즉시 실행한다.
 * 큐에 이미 진행 중인 전이가 있으면 완료 시 _complete가 next를 자동 시작한다.
 *
 * 실행 컨텍스트: SPDK thread에서만 호출 가능 (spdk_get_thread() 검증).
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_start/stop/pause/resume → [nvmf_subsystem_state_change]
 *     → nvmf_subsystem_do_state_change (큐 선두 시)
 */
static int
nvmf_subsystem_state_change(struct spdk_nvmf_subsystem *subsystem,
			    uint32_t nsid,
			    enum spdk_nvmf_subsystem_state requested_state,
			    spdk_nvmf_subsystem_state_change_done cb_fn,
			    void *cb_arg)
{
	struct nvmf_subsystem_state_change_ctx *ctx;	/* [한국어] 새로 생성할 상태 전이 요청 컨텍스트 */
	struct spdk_thread *thread;			/* [한국어] 현재 호출 thread (완료 콜백 디스패치용) */

	thread = spdk_get_thread();			/* [한국어] 현재 SPDK thread 조회 */
	if (thread == NULL) {				/* [한국어] SPDK thread 외부(예: OS thread)에서 호출 시 거부 */
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));			/* [한국어] 상태 전이 요청 ctx 할당 (0 초기화) */
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->subsystem = subsystem;			/* [한국어] 대상 subsystem 역참조 */
	ctx->nsid = nsid;				/* [한국어] pause 대상 NSID (0=전체) */
	ctx->requested_state = requested_state;		/* [한국어] 요청 목표 상태 */
	ctx->cb_fn = cb_fn;				/* [한국어] 완료 시 호출할 콜백 */
	ctx->cb_arg = cb_arg;				/* [한국어] 완료 콜백 인자 */
	ctx->thread = thread;				/* [한국어] 완료 콜백을 실행할 thread */

	pthread_mutex_lock(&subsystem->mutex);		/* [한국어] state_changes TAILQ 접근 직렬화 */
	TAILQ_INSERT_TAIL(&subsystem->state_changes, ctx, link);
	/* [한국어] FIFO 순서로 큐 꼬리에 삽입 (직렬 처리 보장) */
	if (ctx != TAILQ_FIRST(&subsystem->state_changes)) {
		/* [한국어] 선두가 아니면 이미 진행 중인 전이가 있음 — 완료 후 자동 시작되므로 대기 */
		pthread_mutex_unlock(&subsystem->mutex);
		return 0;
	}
	pthread_mutex_unlock(&subsystem->mutex);	/* [한국어] mutex 해제 후 do_state_change 호출 */

	nvmf_subsystem_do_state_change(ctx);		/* [한국어] 큐 선두이므로 즉시 전이 시작 */

	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_start - INACTIVE → ACTIVE 전이 (호스트 connect 수신 시작)
 *
 * @subsystem: 대상 (현재 INACTIVE 가정)
 * @cb_fn:     완료 콜백 (subsystem, cb_arg, status) — status 0=성공
 * @cb_arg:    콜백 인자
 * @return: 0=요청 큐잉 성공, -EINVAL/-ENOMEM
 *
 * NS/listener/host 구성 후 본 함수를 호출하면 모든 poll group이 이 subsystem을
 * 활성화하고 호스트 connect 명령을 받기 시작한다. 비동기로 동작 — cb_fn이
 * 호출되어야 ACTIVE 보장. 내부적으로 nvmf_subsystem_state_change 사용.
 */
int
spdk_nvmf_subsystem_start(struct spdk_nvmf_subsystem *subsystem,
			  spdk_nvmf_subsystem_state_change_done cb_fn,
			  void *cb_arg)
{
	return nvmf_subsystem_state_change(subsystem, 0, SPDK_NVMF_SUBSYSTEM_ACTIVE, cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_stop - ACTIVE → INACTIVE 전이 (모든 ctrlr disconnect)
 *
 * 모든 호스트 컨트롤러가 disconnect 되고, poll group에서도 subsystem이 제거된다.
 * destroy 직전 단계에서 호출. 비동기.
 */
int
spdk_nvmf_subsystem_stop(struct spdk_nvmf_subsystem *subsystem,
			 spdk_nvmf_subsystem_state_change_done cb_fn,
			 void *cb_arg)
{
	return nvmf_subsystem_state_change(subsystem, 0, SPDK_NVMF_SUBSYSTEM_INACTIVE, cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_pause - ACTIVE → PAUSED (특정 NS의 IO만 중단 가능)
 *
 * @nsid: 0=전체 IO 중단, >0=해당 NSID의 IO만 quiesce. NS hot-add/remove 시 사용.
 * paused 상태에서 NS 추가/삭제, host ACL 변경 등 control plane 변경 가능.
 */
int
spdk_nvmf_subsystem_pause(struct spdk_nvmf_subsystem *subsystem,
			  uint32_t nsid,
			  spdk_nvmf_subsystem_state_change_done cb_fn,
			  void *cb_arg)
{
	return nvmf_subsystem_state_change(subsystem, nsid, SPDK_NVMF_SUBSYSTEM_PAUSED, cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_resume - PAUSED → ACTIVE 전이 (IO 재개)
 *
 * pause 후 NS 변경 등이 끝나면 호출하여 모든 poll group의 IO 처리 재개.
 */
int
spdk_nvmf_subsystem_resume(struct spdk_nvmf_subsystem *subsystem,
			   spdk_nvmf_subsystem_state_change_done cb_fn,
			   void *cb_arg)
{
	return nvmf_subsystem_state_change(subsystem, 0, SPDK_NVMF_SUBSYSTEM_ACTIVE, cb_fn, cb_arg);
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_first - tgt의 RB tree에서 첫 번째 subsystem 반환
 *
 * @tgt: 대상 nvmf target
 * @return: NQN 사전순으로 가장 작은 subsystem, 없으면 NULL
 *
 * RB_MIN 매크로는 RB tree의 최소 키(NQN 문자열 비교 기준) 노드를 반환한다.
 * nvmf_rpc.c의 get_subsystems RPC 등에서 전체 subsystem 순회 시작점으로 사용.
 *
 * 호출 체인:
 *   RPC get_subsystems / write_config_json → [spdk_nvmf_subsystem_get_first]
 *     → spdk_nvmf_subsystem_get_next (반복)
 */
struct spdk_nvmf_subsystem *
spdk_nvmf_subsystem_get_first(struct spdk_nvmf_tgt *tgt)
{
	return RB_MIN(subsystem_tree, &tgt->subsystems);	/* [한국어] RB tree 최소 NQN subsystem 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_next - RB tree에서 다음 subsystem 반환
 *
 * @subsystem: 현재 subsystem (NULL이면 NULL 반환)
 * @return: 현재 다음 NQN의 subsystem, 없으면 NULL
 *
 * RB_NEXT 매크로로 in-order traversal의 다음 노드를 반환.
 * spdk_nvmf_subsystem_get_first와 함께 전체 subsystem 순회에 사용.
 */
struct spdk_nvmf_subsystem *
spdk_nvmf_subsystem_get_next(struct spdk_nvmf_subsystem *subsystem)
{
	if (!subsystem) {				/* [한국어] NULL 입력 방어 */
		return NULL;
	}

	return RB_NEXT(subsystem_tree, &tgt->subsystems, subsystem);
	/* [한국어] RB tree에서 현재 노드의 in-order 다음 노드 반환 */
}

/*
 * [한국어]
 * nvmf_ns_add_host - NS 레벨 호스트 ACL에 hostnqn 추가
 *
 * @ns:      대상 namespace (ns->hosts TAILQ에 삽입)
 * @hostnqn: 허용할 호스트 NQN 문자열
 * @return: 0=성공, -ENOMEM=메모리 부족
 *
 * subsystem 전체 ACL(allow_any_host)과 별개로, NS별로도 특정 호스트만
 * 볼 수 있게 제한할 수 있다 (no_auto_visible=true인 NS 대상).
 * spdk_nvmf_ns_add_host → nvmf_ns_visible(visible=true) 경로에서 호출.
 *
 * 실행 컨텍스트: subsystem INACTIVE 또는 PAUSED 상태에서만 호출.
 *
 * 호출 체인:
 *   nvmf_ns_visible(visible=true) → [nvmf_ns_add_host]
 */
static int
nvmf_ns_add_host(struct spdk_nvmf_ns *ns, const char *hostnqn)
{
	struct spdk_nvmf_host *host;			/* [한국어] 새로 생성할 호스트 ACL 엔트리 */

	host = calloc(1, sizeof(*host));		/* [한국어] 0 초기화 호스트 엔트리 할당 */
	if (!host) {
		return -ENOMEM;
	}
	snprintf(host->nqn, sizeof(host->nqn), "%s", hostnqn);	/* [한국어] hostnqn을 고정 크기 버퍼에 안전 복사 */
	TAILQ_INSERT_HEAD(&ns->hosts, host, link);	/* [한국어] NS의 hosts 리스트 선두에 삽입 */
	return 0;
}

/*
 * [한국어]
 * nvmf_ns_remove_host - NS 레벨 호스트 ACL에서 hostnqn 제거
 *
 * @ns:   대상 namespace
 * @host: 제거할 호스트 엔트리 (nvmf_ns_find_host로 이미 탐색됨)
 *
 * TAILQ에서 제거 후 메모리를 해제한다.
 * nvmf_ns_visible(visible=false) 경로에서 호출되며, NS 제거 시에도 호출된다.
 *
 * 호출 체인:
 *   nvmf_ns_visible(visible=false) → [nvmf_ns_remove_host]
 *   spdk_nvmf_subsystem_remove_ns → [nvmf_ns_remove_host] (FOREACH_SAFE)
 */
static void
nvmf_ns_remove_host(struct spdk_nvmf_ns *ns, struct spdk_nvmf_host *host)
{
	TAILQ_REMOVE(&ns->hosts, host, link);		/* [한국어] ns의 hosts TAILQ에서 분리 */
	free(host);					/* [한국어] 호스트 엔트리 메모리 해제 (키 없어 nvmf_host_free 불필요) */
}

/*
 * [한국어]
 * _async_event_ns_notice - ctrlr->thread에서 NS 변경 AER(Async Event Request) 발생
 *
 * @_ctrlr: struct spdk_nvmf_ctrlr* (send_msg 콜백용 void*)
 *
 * spdk_thread_send_msg를 통해 ctrlr->thread로 디스패치되어
 * nvmf_ctrlr_async_event_ns_notice를 해당 thread 컨텍스트에서 안전하게 호출한다.
 * AER는 호스트에게 Namespace Attribute Changed 이벤트를 알리는 NVMe-oF 메커니즘이다.
 *
 * 실행 컨텍스트: ctrlr->thread (send_msg 디스패치됨).
 *
 * 호출 체인:
 *   send_async_event_ns_notice → spdk_thread_send_msg → [_async_event_ns_notice]
 *     → nvmf_ctrlr_async_event_ns_notice (ctrlr.c)
 */
static void
_async_event_ns_notice(void *_ctrlr)
{
	struct spdk_nvmf_ctrlr *ctrlr = _ctrlr;	/* [한국어] void* 인자 → ctrlr 포인터 복원 */

	nvmf_ctrlr_async_event_ns_notice(ctrlr);	/* [한국어] NVMe AER: Namespace Attribute Changed 이벤트 전송 */
}

/*
 * [한국어]
 * send_async_event_ns_notice - NS 변경 AER을 ctrlr thread에 비동기로 전송
 *
 * @ctrlr: 통지 대상 ctrlr (ctrlr->thread에 메시지 발송)
 *
 * NS가 추가/제거/변경될 때 이미 connect된 호스트 컨트롤러들에게 알리는 함수.
 * nvmf_ns_visible에서 ctrlr의 NS visibility가 변경될 때 호출된다.
 * cross-thread safe: ctrlr->thread로 메시지를 보내므로 현재 thread와 무관하게 안전.
 *
 * 호출 체인:
 *   nvmf_ns_visible → [send_async_event_ns_notice] → spdk_thread_send_msg
 *     → _async_event_ns_notice → nvmf_ctrlr_async_event_ns_notice
 */
static void
send_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	spdk_thread_send_msg(ctrlr->thread, _async_event_ns_notice, ctrlr);
	/* [한국어] ctrlr->thread에 메시지 전달 — ctrlr.c의 AER 처리가 올바른 thread에서 실행됨 */
}

/*
 * [한국어]
 * nvmf_ns_visible - 특정 호스트에 대한 NS visibility 설정 (NS 레벨 ACL 조작)
 *
 * @subsystem: 대상 subsystem (INACTIVE 또는 PAUSED 상태 필요)
 * @nsid:      대상 NSID (1 이상 max_nsid 이하)
 * @hostnqn:   변경할 호스트 NQN (NULL 또는 유효하지 않으면 -EINVAL)
 * @visible:   true=허용(add), false=제거(remove)
 * @return: 0=성공, -1=상태 부적합, -EINVAL=파라미터 오류, -ENOENT=NS 없음, -EPERM=always_visible NS
 *
 * 이 함수는 NS별 호스트 visibility를 제어하는 핵심 함수다.
 * NS가 always_visible(no_auto_visible=false로 생성)이면 -EPERM을 반환한다.
 * 다음 두 레벨을 동시에 업데이트한다:
 *   1) ns->hosts TAILQ — 미래에 connect할 호스트를 위한 영속 ACL
 *   2) 현재 connect된 ctrlr의 NS visibility 비트맵 — 즉시 반영
 * visibility 변경 시 해당 ctrlr에 NS Change AER을 발송한다.
 *
 * 실행 컨텍스트: spdk_nvmf_ns_add_host / ns_remove_host 공개 API에서 호출.
 * INACTIVE 또는 PAUSED 상태 강제 (assert로 검증).
 *
 * 호출 체인:
 *   spdk_nvmf_ns_add_host → [nvmf_ns_visible(visible=true)]
 *   spdk_nvmf_ns_remove_host → [nvmf_ns_visible(visible=false)]
 *     → nvmf_ns_add/remove_host, nvmf_ctrlr_ns_set_visible, send_async_event_ns_notice
 */
static int
nvmf_ns_visible(struct spdk_nvmf_subsystem *subsystem,
		uint32_t nsid,
		const char *hostnqn,
		bool visible)
{
	struct spdk_nvmf_ns *ns;		/* [한국어] 대상 NS 객체 */
	struct spdk_nvmf_ctrlr *ctrlr;		/* [한국어] 현재 connect된 ctrlr 순회용 */
	struct spdk_nvmf_host *host;		/* [한국어] NS ACL에서 찾은 호스트 엔트리 */
	int rc;					/* [한국어] nvmf_ns_add_host 반환값 */

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		/* [한국어] ACTIVE 중에는 NS ACL 변경 금지 (race condition 방지) */
		assert(false);
		return -1;
	}

	if (hostnqn == NULL || !nvmf_nqn_is_valid(hostnqn)) {
		/* [한국어] NULL 또는 잘못된 NQN은 거부 */
		return -EINVAL;
	}

	if (nsid == 0 || nsid > subsystem->max_nsid) {
		/* [한국어] NSID 범위 검증 (1~max_nsid) */
		return -EINVAL;
	}

	ns = subsystem->ns[nsid - 1];		/* [한국어] NSID → 0-based 인덱스로 NS 포인터 조회 */
	if (!ns) {
		return -ENOENT;			/* [한국어] 해당 NSID에 NS가 없음 */
	}

	if (ns->always_visible) {
		/* No individual host control */
		/* [한국어] always_visible NS는 모든 호스트에 노출 — 개별 호스트 제어 불가 (-EPERM) */
		return -EPERM;
	}

	/* Save host info to use for any future controllers. */
	host = nvmf_ns_find_host(ns, hostnqn);	/* [한국어] ns->hosts에서 hostnqn 검색 */
	if (visible && host == NULL) {		/* [한국어] 허용 요청인데 ACL에 없으면 추가 */
		rc = nvmf_ns_add_host(ns, hostnqn);
		if (rc) {
			return rc;
		}
	} else if (!visible && host != NULL) {	/* [한국어] 제거 요청인데 ACL에 있으면 제거 */
		nvmf_ns_remove_host(ns, host);
	}

	/* Also apply to existing controllers. */
	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		/* [한국어] 현재 connect된 모든 ctrlr에도 즉시 적용 */
		if (strcmp(hostnqn, ctrlr->hostnqn) ||
		    nvmf_ctrlr_ns_is_visible(ctrlr, nsid) == visible) {
			/* [한국어] hostnqn 불일치 또는 이미 원하는 visibility이면 skip */
			continue;
		}
		nvmf_ctrlr_ns_set_visible(ctrlr, nsid, visible);
		/* [한국어] ctrlr의 NS visibility 비트맵 갱신 */
		send_async_event_ns_notice(ctrlr);
		/* [한국어] 호스트에게 NS 변경 AER 발송 (재스캔 유도) */
		nvmf_ctrlr_ns_changed(ctrlr, nsid);
		/* [한국어] changed_nsids 배열 업데이트 (Identify Active NS 응답에 반영) */
	}

	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_ns_add_host - NS에 특정 호스트 visibility 허용 (공개 API)
 *
 * @subsystem: 대상 subsystem
 * @nsid:      대상 NSID
 * @hostnqn:   허용할 호스트 NQN
 * @flags:     현재 미사용 (향후 확장용 예약)
 * @return: 0=성공, 음수=오류
 *
 * no_auto_visible=true로 생성된 NS에 특정 호스트를 개별 허용하는 공개 API.
 * RPC nvmf_ns_add_host 핸들러에서 호출된다.
 * 내부적으로 nvmf_ns_visible(visible=true) 위임.
 *
 * 호출 체인:
 *   RPC nvmf_ns_add_host → [spdk_nvmf_ns_add_host] → nvmf_ns_visible(true)
 */
int
spdk_nvmf_ns_add_host(struct spdk_nvmf_subsystem *subsystem,
		      uint32_t nsid,
		      const char *hostnqn,
		      uint32_t flags)
{
	SPDK_DTRACE_PROBE4(spdk_nvmf_ns_add_host,
			   subsystem->subnqn,
			   nsid,
			   hostnqn,
			   flags);
	/* [한국어] USDT trace: NS 호스트 추가 이벤트 (NQN, nsid, hostnqn, flags) */
	return nvmf_ns_visible(subsystem, nsid, hostnqn, true);
}

/*
 * [한국어]
 * spdk_nvmf_ns_remove_host - NS에서 특정 호스트 visibility 제거 (공개 API)
 *
 * @subsystem: 대상 subsystem
 * @nsid:      대상 NSID
 * @hostnqn:   제거할 호스트 NQN
 * @flags:     현재 미사용 (향후 확장용 예약)
 * @return: 0=성공, 음수=오류
 *
 * 기존에 허용된 호스트를 NS에서 개별 제거하는 공개 API.
 * RPC nvmf_ns_remove_host 핸들러에서 호출된다.
 * 내부적으로 nvmf_ns_visible(visible=false) 위임.
 *
 * 호출 체인:
 *   RPC nvmf_ns_remove_host → [spdk_nvmf_ns_remove_host] → nvmf_ns_visible(false)
 */
int
spdk_nvmf_ns_remove_host(struct spdk_nvmf_subsystem *subsystem,
			 uint32_t nsid,
			 const char *hostnqn,
			 uint32_t flags)
{
	SPDK_DTRACE_PROBE4(spdk_nvmf_ns_remove_host,
			   subsystem->subnqn,
			   nsid,
			   hostnqn,
			   flags);
	/* [한국어] USDT trace: NS 호스트 제거 이벤트 */
	return nvmf_ns_visible(subsystem, nsid, hostnqn, false);
}

/* Must hold subsystem->mutex while calling this function */
/*
 * [한국어]
 * nvmf_subsystem_find_host - subsystem의 hosts TAILQ에서 hostnqn 검색
 *
 * @subsystem: 대상 subsystem (반드시 mutex 보유 상태에서 호출)
 * @hostnqn:   찾을 호스트 NQN 문자열
 * @return: 일치하는 호스트 엔트리, 없으면 NULL
 *
 * subsystem 레벨 ACL(allow_any_host=false 시 유효)에서 hostnqn을 선형 탐색.
 * hosts 리스트의 크기는 일반적으로 수십 개 미만이라 O(N) 선형 탐색으로 충분.
 * 주의: 반드시 pthread_mutex_lock(&subsystem->mutex) 보유 상태에서 호출.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_add_host_ext → [nvmf_subsystem_find_host] (중복 체크)
 *   spdk_nvmf_subsystem_remove_host → [nvmf_subsystem_find_host]
 *   spdk_nvmf_subsystem_host_allowed → [nvmf_subsystem_find_host]
 */
static struct spdk_nvmf_host *
nvmf_subsystem_find_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host = NULL;		/* [한국어] 순회용 포인터 (초기 NULL) */

	TAILQ_FOREACH(host, &subsystem->hosts, link) {	/* [한국어] hosts TAILQ 선형 탐색 */
		if (strcmp(hostnqn, host->nqn) == 0) {	/* [한국어] NQN 문자열 완전 일치 확인 */
			return host;			/* [한국어] 찾았으면 즉시 반환 */
		}
	}

	return NULL;					/* [한국어] 미등록 호스트 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_add_host_ext - 호스트 NQN을 ACL에 추가 (옵션 지원)
 *
 * @subsystem: 대상 subsystem
 * @hostnqn:   허용할 호스트 NQN (NVMe-oF connect 시 사용된 hostnqn과 비교)
 * @opts:      DH-HMAC-CHAP 키 등 호스트별 인증 옵션
 * @return: 0=성공, -EINVAL(NQN 부적합/이미 존재), -ENOMEM
 *
 * subsystem이 allow_any_host=false일 때만 의미가 있다 (true이면 ACL 무시).
 * 인증 키(dhchap_key/dhchap_ctrlr_key)는 spdk_keyring에 등록된 핸들로
 * 전달되며, 본 함수에서 ref count를 dup한다.
 *
 * 호출 후 모든 transport에 add_host 콜백을 호출하여 transport별 사전 작업
 * (예: TLS PSK 등록)을 수행한다.
 *
 * 호출 체인:
 *   RPC subsystem_add_host → [spdk_nvmf_subsystem_add_host_ext]
 *     → nvmf_nqn_is_valid → spdk_key_dup → transport->subsystem_add_host (각 transport)
 */
int
spdk_nvmf_subsystem_add_host_ext(struct spdk_nvmf_subsystem *subsystem,
				 const char *hostnqn, struct spdk_nvmf_host_opts *opts)
{
	struct spdk_nvmf_host *host;
	struct spdk_nvmf_transport *transport;
	struct spdk_key *key;
	int rc;

	if (!nvmf_nqn_is_valid(hostnqn)) {
		return -EINVAL;
	}

	pthread_mutex_lock(&subsystem->mutex);

	if (nvmf_subsystem_find_host(subsystem, hostnqn)) {
		/* This subsystem already allows the specified host. */
		pthread_mutex_unlock(&subsystem->mutex);
		return -EINVAL;
	}

	host = calloc(1, sizeof(*host));
	if (!host) {
		pthread_mutex_unlock(&subsystem->mutex);
		return -ENOMEM;
	}

	key = SPDK_GET_FIELD(opts, dhchap_key, NULL);
	if (key != NULL) {
		if (!nvmf_auth_is_supported()) {
			SPDK_ERRLOG("NVMe in-band authentication is unsupported\n");
			pthread_mutex_unlock(&subsystem->mutex);
			nvmf_host_free(host);
			return -EINVAL;
		}
		host->dhchap_key = spdk_key_dup(key);
		if (host->dhchap_key == NULL) {
			pthread_mutex_unlock(&subsystem->mutex);
			nvmf_host_free(host);
			return -EINVAL;
		}
		key = SPDK_GET_FIELD(opts, dhchap_ctrlr_key, NULL);
		if (key != NULL) {
			host->dhchap_ctrlr_key = spdk_key_dup(key);
			if (host->dhchap_ctrlr_key == NULL) {
				pthread_mutex_unlock(&subsystem->mutex);
				nvmf_host_free(host);
				return -EINVAL;
			}
		}
	} else if (SPDK_GET_FIELD(opts, dhchap_ctrlr_key, NULL) != NULL) {
		SPDK_ERRLOG("DH-HMAC-CHAP controller key requires host key to be set\n");
		pthread_mutex_unlock(&subsystem->mutex);
		nvmf_host_free(host);
		return -EINVAL;
	}

	snprintf(host->nqn, sizeof(host->nqn), "%s", hostnqn);

	SPDK_DTRACE_PROBE2(nvmf_subsystem_add_host, subsystem->subnqn, host->nqn);

	TAILQ_INSERT_HEAD(&subsystem->hosts, host, link);

	if (!TAILQ_EMPTY(&subsystem->listeners)) {
		spdk_nvmf_send_discovery_log_notice(subsystem->tgt, hostnqn);
	}

	for (transport = spdk_nvmf_transport_get_first(subsystem->tgt); transport;
	     transport = spdk_nvmf_transport_get_next(transport)) {
		if (transport->ops->subsystem_add_host) {
			rc = transport->ops->subsystem_add_host(transport, subsystem, hostnqn,
								SPDK_GET_FIELD(opts, params, NULL));
			if (rc) {
				SPDK_ERRLOG("Unable to add host to %s transport\n", transport->ops->name);
				/* Remove this host from all transports we've managed to add it to. */
				pthread_mutex_unlock(&subsystem->mutex);
				spdk_nvmf_subsystem_remove_host(subsystem, hostnqn);
				return rc;
			}
		}
	}

	pthread_mutex_unlock(&subsystem->mutex);

	return 0;
}

int
spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn,
			     const struct spdk_json_val *params)
{
	struct spdk_nvmf_host_opts opts = {};

	opts.size = SPDK_SIZEOF(&opts, params);
	opts.params = params;

	return spdk_nvmf_subsystem_add_host_ext(subsystem, hostnqn, &opts);
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_remove_host - 호스트 NQN을 ACL에서 제거
 *
 * @return: 0=성공, -ENOENT(미등록)
 *
 * 등록된 host 객체를 free하고 listener가 있으면 discovery log notice를 통해
 * 변경을 알린다. 각 transport에도 remove_host 콜백 통지.
 * 주의: 이미 connect된 ctrlr는 자동 disconnect되지 않으며, 호스트가 다음에
 * connect를 시도할 때 차단된다. 즉시 끊으려면 disconnect_host 별도 호출.
 */
int
spdk_nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host;
	struct spdk_nvmf_transport *transport;

	pthread_mutex_lock(&subsystem->mutex);

	host = nvmf_subsystem_find_host(subsystem, hostnqn);
	if (host == NULL) {
		pthread_mutex_unlock(&subsystem->mutex);
		return -ENOENT;
	}

	SPDK_DTRACE_PROBE2(nvmf_subsystem_remove_host, subsystem->subnqn, host->nqn);

	nvmf_subsystem_remove_host(subsystem, host);

	if (!TAILQ_EMPTY(&subsystem->listeners)) {
		spdk_nvmf_send_discovery_log_notice(subsystem->tgt, hostnqn);
	}

	for (transport = spdk_nvmf_transport_get_first(subsystem->tgt); transport;
	     transport = spdk_nvmf_transport_get_next(transport)) {
		if (transport->ops->subsystem_remove_host) {
			transport->ops->subsystem_remove_host(transport, subsystem, hostnqn);
		}
	}

	pthread_mutex_unlock(&subsystem->mutex);

	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_set_keys - 등록된 호스트의 DH-HMAC-CHAP 인증 키 교체
 *
 * @subsystem: 대상 subsystem
 * @hostnqn:   키를 교체할 호스트 NQN (이미 ACL에 등록되어 있어야 함)
 * @opts:      새 키 옵션 (dhchap_key: 호스트 인증키, dhchap_ctrlr_key: 컨트롤러 인증키)
 * @return: 0=성공, -EINVAL=인증 미지원/호스트 미등록/ctrlr_key만 지정, -ENOMEM(spdk_key_dup 실패)
 *
 * DH-HMAC-CHAP(Diffie-Hellman Hash-based Message Authentication Code Challenge-response)
 * 인증 키를 기존 호스트에 대해 runtime에 교체한다. TLS PSK와 달리 DH-HMAC-CHAP는
 * connect 시마다 challenge-response 교환을 통해 인증한다.
 * dhchap_ctrlr_key만 단독으로 지정하는 것은 스펙상 불허 (host key 없이 ctrlr 인증 불가).
 *
 * 실행 컨텍스트: subsystem->mutex 보호 하에 host 객체 접근.
 *
 * 호출 체인:
 *   RPC nvmf_subsystem_set_keys → [spdk_nvmf_subsystem_set_keys]
 *     → spdk_key_dup (새 키 참조 획득) → spdk_keyring_put_key (이전 키 참조 반환)
 */
int
spdk_nvmf_subsystem_set_keys(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn,
			     struct spdk_nvmf_subsystem_key_opts *opts)
{
	struct spdk_nvmf_host *host;		/* [한국어] 키를 교체할 호스트 엔트리 */
	struct spdk_key *key, *ckey;		/* [한국어] 새 host key / ctrlr key (dup 후 임시 보관) */

	if (!nvmf_auth_is_supported()) {	/* [한국어] DH-HMAC-CHAP 지원 여부 확인 (컴파일 옵션 또는 OpenSSL 유무) */
		SPDK_ERRLOG("NVMe in-band authentication is unsupported\n");
		return -EINVAL;
	}

	pthread_mutex_lock(&subsystem->mutex);	/* [한국어] host 객체 접근 및 키 교체 원자화 */
	host = nvmf_subsystem_find_host(subsystem, hostnqn);
	/* [한국어] hostnqn으로 ACL에서 호스트 엔트리 탐색 */
	if (host == NULL) {
		pthread_mutex_unlock(&subsystem->mutex);
		return -EINVAL;			/* [한국어] 미등록 호스트 — 키 교체 불가 */
	}

	if (SPDK_GET_FIELD(opts, dhchap_key, host->dhchap_key) == NULL &&
	    SPDK_GET_FIELD(opts, dhchap_ctrlr_key, host->dhchap_ctrlr_key) != NULL) {
		/* [한국어] ctrlr_key만 있고 host_key가 없으면 NVMe-oF 인증 스펙 위반 */
		SPDK_ERRLOG("DH-HMAC-CHAP controller key requires host key to be set\n");
		pthread_mutex_unlock(&subsystem->mutex);
		return -EINVAL;
	}
	key = SPDK_GET_FIELD(opts, dhchap_key, NULL);	/* [한국어] opts에 dhchap_key 필드가 있으면 추출, 없으면 NULL */
	if (key != NULL) {
		key = spdk_key_dup(key);		/* [한국어] keyring ref count 증가 — 실패 시 keyring이 해제됨 방지 */
		if (key == NULL) {
			pthread_mutex_unlock(&subsystem->mutex);
			return -EINVAL;
		}
	}
	ckey = SPDK_GET_FIELD(opts, dhchap_ctrlr_key, NULL);	/* [한국어] 컨트롤러 인증키 추출 */
	if (ckey != NULL) {
		ckey = spdk_key_dup(ckey);		/* [한국어] ctrlr key ref count 증가 */
		if (ckey == NULL) {
			pthread_mutex_unlock(&subsystem->mutex);
			spdk_keyring_put_key(key);	/* [한국어] 이미 dup한 key 참조 반환 (롤백) */
			return -EINVAL;
		}
	}
	if (SPDK_FIELD_VALID(opts, dhchap_key)) {		/* [한국어] opts 구조체 크기가 dhchap_key 필드를 포함하면 교체 */
		spdk_keyring_put_key(host->dhchap_key);		/* [한국어] 이전 key 참조 반환 */
		host->dhchap_key = key;				/* [한국어] 새 key로 교체 */
	}
	if (SPDK_FIELD_VALID(opts, dhchap_ctrlr_key)) {		/* [한국어] opts 구조체 크기가 ctrlr_key 필드를 포함하면 교체 */
		spdk_keyring_put_key(host->dhchap_ctrlr_key);	/* [한국어] 이전 ctrlr key 참조 반환 */
		host->dhchap_ctrlr_key = ckey;			/* [한국어] 새 ctrlr key로 교체 */
	}
	pthread_mutex_unlock(&subsystem->mutex);		/* [한국어] 키 교체 완료, mutex 해제 */

	return 0;
}

struct nvmf_subsystem_disconnect_host_ctx {
	struct spdk_nvmf_subsystem		*subsystem;
	/* [한국어] disconnect 대상 subsystem.
	 * 설정자: spdk_nvmf_subsystem_disconnect_host에서 calloc 후 초기화.
	 * 읽는 자: nvmf_subsystem_disconnect_qpairs_by_host에서 subsys 비교,
	 *          nvmf_subsystem_disconnect_host_fini에서 완료 처리.
	 * 값 범위: 유효한 subsystem 포인터 (해제 전까지 참조 유효).
	 * 동기화: for_each_channel 내부 직렬화, 별도 락 불필요. */

	char					*hostnqn;
	/* [한국어] disconnect할 hostnqn의 동적 복사본 (strdup).
	 * 설정자: spdk_nvmf_subsystem_disconnect_host에서 strdup.
	 * 읽는 자: nvmf_subsystem_disconnect_qpairs_by_host에서 strncmp 비교,
	 *          nvmf_subsystem_disconnect_host_fini에서 free.
	 * 값 범위: NULL 종결 NQN 문자열 (max SPDK_NVMF_NQN_MAX_LEN+1).
	 * 동기화: for_each_channel 실행 중에는 불변 (읽기 전용). */

	spdk_nvmf_tgt_subsystem_listen_done_fn	cb_fn;
	/* [한국어] disconnect 완료 시 호출할 콜백 함수.
	 * 설정자: spdk_nvmf_subsystem_disconnect_host에서 초기화.
	 * 읽는 자: nvmf_subsystem_disconnect_host_fini에서 NULL 체크 후 호출.
	 * 값 범위: 함수 포인터 또는 NULL (콜백 없이도 동작 가능).
	 * 동기화: for_each_channel 완료 후 단일 호출. */

	void					*cb_arg;
	/* [한국어] cb_fn에 전달할 불투명 인자.
	 * 설정자: spdk_nvmf_subsystem_disconnect_host에서 초기화.
	 * 읽는 자: nvmf_subsystem_disconnect_host_fini에서 cb_fn 호출 시 전달.
	 * 값 범위: NULL 포함 임의의 포인터.
	 * 동기화: for_each_channel 완료 후 단일 접근. */
};

/*
 * [한국어]
 * nvmf_subsystem_disconnect_host_fini - 모든 poll group 순회 완료 후 ctx 정리 및 콜백 호출
 *
 * @i:      spdk_for_each_channel 이터레이터
 * @status: 0=성공 (disconnect 명령은 항상 성공으로 간주)
 *
 * nvmf_subsystem_disconnect_qpairs_by_host가 모든 poll group을 순회 완료 후
 * 이 함수가 호출된다. ctx 메모리를 해제하고 완료 콜백을 통지한다.
 * 주의: disconnect 요청을 보냈을 뿐, 실제 qpair disconnect 완료를 기다리지 않는다.
 *
 * 호출 체인:
 *   spdk_for_each_channel (complete) → [nvmf_subsystem_disconnect_host_fini]
 */
static void
nvmf_subsystem_disconnect_host_fini(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_subsystem_disconnect_host_ctx *ctx;	/* [한국어] 완료할 ctx */

	ctx = spdk_io_channel_iter_get_ctx(i);		/* [한국어] 이터레이터에서 ctx 추출 */

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, status);	/* [한국어] 완료 콜백 호출 (status=0 항상) */
	}
	free(ctx->hostnqn);				/* [한국어] strdup으로 할당한 hostnqn 문자열 해제 */
	free(ctx);					/* [한국어] ctx 구조체 해제 */
}

/*
 * [한국어]
 * nvmf_subsystem_disconnect_qpairs_by_host - 각 poll group에서 hostnqn의 qpair disconnect
 *
 * @i: spdk_for_each_channel 이터레이터 (ctx, 현재 io_channel 담고 있음)
 *
 * 각 poll group에서 해당 subsystem에 connect된 qpair 중 hostnqn이 일치하는
 * 모든 qpair에 disconnect를 요청한다.
 * 주의: disconnect 요청(spdk_nvmf_qpair_disconnect)은 fire-and-forget이다.
 * 실제 완료를 기다리지 않고 spdk_for_each_channel_continue로 즉시 다음 poll group으로 이동.
 * TAILQ_FOREACH_SAFE를 사용해 disconnect 중 qpair 제거로 인한 이터레이터 무효화 방지.
 *
 * 실행 컨텍스트: 각 poll group thread.
 *
 * 호출 체인:
 *   spdk_for_each_channel → [nvmf_subsystem_disconnect_qpairs_by_host]
 *     → spdk_nvmf_qpair_disconnect → spdk_for_each_channel_continue
 */
static void
nvmf_subsystem_disconnect_qpairs_by_host(struct spdk_io_channel_iter *i)
{
	struct nvmf_subsystem_disconnect_host_ctx *ctx;		/* [한국어] disconnect 요청 ctx */
	struct spdk_nvmf_poll_group *group;			/* [한국어] 현재 poll group */
	struct spdk_io_channel *ch;				/* [한국어] 현재 io_channel */
	struct spdk_nvmf_qpair *qpair, *tmp_qpair;		/* [한국어] 순회 qpair / SAFE 임시 포인터 */
	struct spdk_nvmf_ctrlr *ctrlr;				/* [한국어] qpair의 ctrlr */

	ctx = spdk_io_channel_iter_get_ctx(i);			/* [한국어] ctx 추출 */
	ch = spdk_io_channel_iter_get_channel(i);		/* [한국어] 현재 채널 (poll group 1:1) */
	group = spdk_io_channel_get_ctx(ch);			/* [한국어] poll group 포인터 획득 */

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, tmp_qpair) {
		/* [한국어] SAFE 버전 사용 — disconnect 중 qpair 제거 시 이터레이터 보호 */
		ctrlr = qpair->ctrlr;				/* [한국어] qpair에 연결된 ctrlr 참조 */

		if (ctrlr == NULL || ctrlr->subsys != ctx->subsystem) {
			/* [한국어] ctrlr 없거나 다른 subsystem의 qpair는 무시 */
			continue;
		}

		if (strncmp(ctrlr->hostnqn, ctx->hostnqn, sizeof(ctrlr->hostnqn)) == 0) {
			/* [한국어] hostnqn 일치 — 이 qpair를 disconnect 요청 */
			/* Right now this does not wait for the queue pairs to actually disconnect. */
			spdk_nvmf_qpair_disconnect(qpair);	/* [한국어] fire-and-forget: 비동기 disconnect 트리거 */
		}
	}
	spdk_for_each_channel_continue(i, 0);			/* [한국어] 이 poll group 처리 완료, 다음으로 진행 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_disconnect_host - 특정 hostnqn의 모든 qpair disconnect (공개 API)
 *
 * @subsystem: 대상 subsystem
 * @hostnqn:   disconnect할 호스트 NQN
 * @cb_fn:     모든 poll group 순회 완료 시 호출 콜백 (NULL 가능)
 * @cb_arg:    콜백 인자
 * @return: 0=비동기 시작 성공, -ENOMEM=ctx 할당 실패
 *
 * ACL에서 호스트를 제거해도 이미 connect된 qpair는 자동 종료되지 않는다.
 * 이 함수는 모든 poll group을 순회하며 hostnqn이 일치하는 qpair에 disconnect를
 * 요청한다. 단, 실제 disconnect 완료 시점과 cb_fn 호출 시점이 다를 수 있다
 * (disconnect는 fire-and-forget, cb_fn은 순회 완료 시점에 호출됨).
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_remove_host → (선택적) [spdk_nvmf_subsystem_disconnect_host]
 *   RPC nvmf_subsystem_remove_listener → (선택적) [spdk_nvmf_subsystem_disconnect_host]
 */
int
spdk_nvmf_subsystem_disconnect_host(struct spdk_nvmf_subsystem *subsystem,
				    const char *hostnqn,
				    spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				    void *cb_arg)
{
	struct nvmf_subsystem_disconnect_host_ctx *ctx;	/* [한국어] for_each_channel에 전달할 ctx */

	ctx = calloc(1, sizeof(struct nvmf_subsystem_disconnect_host_ctx));
	/* [한국어] 0 초기화 ctx 할당 */
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->hostnqn = strdup(hostnqn);			/* [한국어] for_each_channel 순회 동안 유효해야 하므로 복사 */
	if (ctx->hostnqn == NULL) {
		free(ctx);
		return -ENOMEM;
	}

	ctx->subsystem = subsystem;			/* [한국어] 대상 subsystem */
	ctx->cb_fn = cb_fn;				/* [한국어] 완료 콜백 */
	ctx->cb_arg = cb_arg;				/* [한국어] 완료 콜백 인자 */

	spdk_for_each_channel(subsystem->tgt, nvmf_subsystem_disconnect_qpairs_by_host, ctx,
			      nvmf_subsystem_disconnect_host_fini);
	/* [한국어] 모든 poll group을 순회하며 hostnqn 일치 qpair disconnect,
	 * 완료 시 fini에서 ctx 정리 및 cb_fn 호출 */

	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_set_allow_any_host - subsystem의 allow_any_host 플래그 설정
 *
 * @subsystem:      대상 subsystem
 * @allow_any_host: true=모든 호스트 허용 (ACL 무시), false=ACL 기반 허용
 * @return: 0=성공 (이미 같은 값이면 no-op)
 *
 * NVMe-oF에서 호스트 접근 제어 정책 설정. true이면 hostnqn ACL을 무시하고
 * 모든 호스트에게 connect를 허용한다 (개발/테스트 환경 또는 공개 subsystem).
 * 변경 시 listener가 있으면 discovery log notice를 발송한다.
 * 실행 컨텍스트: 임의 thread (mutex로 동기화).
 */
int
spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem, bool allow_any_host)
{
	if (subsystem->allow_any_host == allow_any_host) {	/* [한국어] 이미 원하는 값이면 no-op */
		return 0;
	}

	pthread_mutex_lock(&subsystem->mutex);			/* [한국어] allow_any_host 쓰기 직렬화 */
	subsystem->allow_any_host = allow_any_host;		/* [한국어] 플래그 갱신 */
	if (!TAILQ_EMPTY(&subsystem->listeners)) {
		spdk_nvmf_send_discovery_log_notice(subsystem->tgt, NULL);
		/* [한국어] listener가 있으면 호스트에게 discovery log 변경 AER 발송 */
	}
	pthread_mutex_unlock(&subsystem->mutex);		/* [한국어] mutex 해제 */

	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_allow_any_host - allow_any_host 플래그 조회
 *
 * @subsystem: 조회할 subsystem (const로 선언되었으나 mutex 취득 필요)
 * @return: true=모든 호스트 허용, false=ACL 기반 허용
 *
 * const 파라미터를 받지만 내부적으로 mutex를 취득해야 하므로
 * const_cast로 우회한다 (영어 주석에 설명됨).
 */
bool
spdk_nvmf_subsystem_get_allow_any_host(const struct spdk_nvmf_subsystem *subsystem)
{
	bool allow_any_host;				/* [한국어] 반환할 플래그 값 */
	struct spdk_nvmf_subsystem *sub;		/* [한국어] const_cast 대상 포인터 */

	/* Technically, taking the mutex modifies data in the subsystem. But the const
	 * is still important to convey that this doesn't mutate any other data. Cast
	 * it away to work around this. */
	sub = (struct spdk_nvmf_subsystem *)subsystem;	/* [한국어] const 제거 캐스팅 (mutex만 수정, 데이터 불변) */

	pthread_mutex_lock(&sub->mutex);		/* [한국어] allow_any_host 읽기 원자화 */
	allow_any_host = sub->allow_any_host;		/* [한국어] 플래그 값 스냅샷 */
	pthread_mutex_unlock(&sub->mutex);		/* [한국어] mutex 해제 */

	return allow_any_host;
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_host_allowed - hostnqn이 이 subsystem에 connect 허용되는지 확인
 *
 * @subsystem: 대상 subsystem
 * @hostnqn:   확인할 호스트 NQN (NULL이면 false)
 * @return: true=허용, false=거부
 *
 * nvmf_ctrlr_connect 시 호스트 접근 제어 검사에 사용된다.
 * allow_any_host=true이면 ACL 검색 없이 즉시 허용.
 * 실행 컨텍스트: ctrlr connect 처리 스레드 (mutex로 동기화).
 *
 * 호출 체인:
 *   nvmf_ctrlr_cmd_connect → [spdk_nvmf_subsystem_host_allowed]
 */
bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	bool allowed;					/* [한국어] 결과 값 */

	if (!hostnqn) {				/* [한국어] NULL hostnqn은 항상 거부 */
		return false;
	}

	pthread_mutex_lock(&subsystem->mutex);		/* [한국어] hosts 리스트 접근 직렬화 */

	if (subsystem->allow_any_host) {		/* [한국어] 모든 호스트 허용 모드 — 즉시 true 반환 */
		pthread_mutex_unlock(&subsystem->mutex);
		return true;
	}

	allowed = nvmf_subsystem_find_host(subsystem, hostnqn) != NULL;
	/* [한국어] ACL 검색 — 등록된 호스트면 true */
	pthread_mutex_unlock(&subsystem->mutex);	/* [한국어] mutex 해제 */

	return allowed;
}

/*
 * [한국어]
 * nvmf_subsystem_host_auth_required - 특정 호스트에 대해 DH-HMAC-CHAP 인증이 필요한지 확인
 *
 * @subsystem: 대상 subsystem
 * @hostnqn:   확인할 호스트 NQN
 * @return: true=인증 필요 (dhchap_key가 설정됨), false=인증 불필요
 *
 * nvmf_ctrlr_connect 중 인증 단계 진입 여부 결정에 사용된다.
 * host가 ACL에 있더라도 dhchap_key가 설정되지 않았으면 인증 불필요.
 */
bool
nvmf_subsystem_host_auth_required(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host;			/* [한국어] ACL에서 찾은 호스트 */
	bool status;					/* [한국어] 인증 필요 여부 */

	pthread_mutex_lock(&subsystem->mutex);		/* [한국어] host→dhchap_key 읽기 직렬화 */
	host = nvmf_subsystem_find_host(subsystem, hostnqn);	/* [한국어] ACL 검색 */
	status = host != NULL && host->dhchap_key != NULL;
	/* [한국어] 호스트가 ACL에 있고 dhchap_key가 설정된 경우에만 인증 필요 */
	pthread_mutex_unlock(&subsystem->mutex);	/* [한국어] mutex 해제 */

	return status;
}

/*
 * [한국어]
 * nvmf_subsystem_get_dhchap_key - hostnqn에 해당하는 DH-HMAC-CHAP 키 반환
 *
 * @subsystem: 대상 subsystem
 * @hostnqn:   키를 조회할 호스트 NQN
 * @type:      NVMF_AUTH_KEY_HOST(호스트 인증) 또는 NVMF_AUTH_KEY_CTRLR(컨트롤러 인증)
 * @return: 성공 시 spdk_key* (ref count 증가됨), 없으면 NULL
 *
 * nvmf_ctrlr 인증 단계에서 HMAC 계산에 사용할 PSK를 가져온다.
 * spdk_key_dup으로 ref count를 증가시키므로 호출자가 사용 완료 후
 * spdk_keyring_put_key로 반환해야 한다.
 */
struct spdk_key *
nvmf_subsystem_get_dhchap_key(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn,
			      enum nvmf_auth_key_type type)
{
	struct spdk_nvmf_host *host;			/* [한국어] ACL에서 찾은 호스트 */
	struct spdk_key *key = NULL;			/* [한국어] 반환할 키 핸들 */

	pthread_mutex_lock(&subsystem->mutex);		/* [한국어] host→dhchap_key 접근 직렬화 */
	host = nvmf_subsystem_find_host(subsystem, hostnqn);	/* [한국어] ACL 검색 */
	if (host != NULL) {
		switch (type) {
		case NVMF_AUTH_KEY_HOST:		/* [한국어] 호스트 인증용 키 (NI2H 방향) */
			key = host->dhchap_key;
			break;
		case NVMF_AUTH_KEY_CTRLR:		/* [한국어] 컨트롤러 인증용 키 (NH2I 방향, 상호 인증) */
			key = host->dhchap_ctrlr_key;
			break;
		}
		if (key != NULL) {
			key = spdk_key_dup(key);	/* [한국어] ref count 증가 — 호출자가 put 필요 */
		}
	}
	pthread_mutex_unlock(&subsystem->mutex);	/* [한국어] mutex 해제 */

	return key;
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_first_host - subsystem의 첫 번째 호스트 ACL 엔트리 반환
 *
 * @subsystem: 조회할 subsystem
 * @return: 첫 번째 호스트 엔트리, 없으면 NULL
 *
 * 호스트 목록을 순회하기 위한 시작점. nvmf_rpc.c의 get_subsystems RPC에서 사용.
 */
struct spdk_nvmf_host *
spdk_nvmf_subsystem_get_first_host(struct spdk_nvmf_subsystem *subsystem)
{
	return TAILQ_FIRST(&subsystem->hosts);		/* [한국어] hosts TAILQ의 첫 번째 엔트리 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_next_host - 다음 호스트 ACL 엔트리 반환
 *
 * @subsystem:  조회할 subsystem (미사용이나 API 일관성 유지)
 * @prev_host:  현재 호스트 엔트리
 * @return: 다음 호스트 엔트리, 없으면 NULL
 */
struct spdk_nvmf_host *
spdk_nvmf_subsystem_get_next_host(struct spdk_nvmf_subsystem *subsystem,
				  struct spdk_nvmf_host *prev_host)
{
	return TAILQ_NEXT(prev_host, link);		/* [한국어] TAILQ에서 다음 엔트리 */
}

/*
 * [한국어]
 * spdk_nvmf_host_get_nqn - 호스트 엔트리에서 NQN 문자열 반환
 *
 * @host: 호스트 엔트리
 * @return: 호스트 NQN 문자열 (NULL 종결, host 객체 수명 동안 유효)
 *
 * nvmf_rpc.c 등에서 호스트 목록 직렬화 시 사용.
 */
const char *
spdk_nvmf_host_get_nqn(const struct spdk_nvmf_host *host)
{
	return host->nqn;				/* [한국어] 고정 크기 배열에 저장된 NQN 직접 반환 */
}

/*
 * [한국어]
 * nvmf_subsystem_find_listener - subsystem의 listener 목록에서 trid 일치 listener 탐색
 *
 * @subsystem: 탐색할 subsystem
 * @trid:      찾을 transport ID (trtype+traddr+trsvcid 조합)
 * @return: 일치하는 활성 listener, 없으면 NULL
 *
 * 비활성(trid=NULL) listener는 건너뛰고 활성 listener만 탐색한다.
 * subsystem_add_listener에서 중복 체크, ctrlr_connect에서 listener 검증 등에 사용.
 */
struct spdk_nvmf_subsystem_listener *
nvmf_subsystem_find_listener(struct spdk_nvmf_subsystem *subsystem,
			     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_subsystem_listener *listener;	/* [한국어] 탐색 중인 listener 포인터 */

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (!nvmf_subsystem_listener_is_active(listener)) {
			continue;	/* [한국어] trid가 NULL인 비활성(stopped) listener 건너뜀 */
		}

		if (spdk_nvme_transport_id_compare(listener->trid, trid) == 0) {
			return listener;	/* [한국어] trid 완전 일치 — 해당 listener 반환 */
		}
	}

	return NULL;	/* [한국어] 목록에 없음 */
}

/*
 * [한국어]
 * nvmf_subsystem_listener_is_active - listener가 활성(active) 상태인지 확인
 *
 * @listener: 확인할 listener (NULL도 허용)
 * @return: true=활성, false=비활성(NULL 또는 trid=NULL)
 *
 * listener가 stop 처리되면 trid가 NULL로 설정된다.
 * listener가 NULL이거나 trid가 NULL이면 비활성으로 간주한다.
 */
bool
nvmf_subsystem_listener_is_active(const struct spdk_nvmf_subsystem_listener *listener)
{
	if (!listener) {			/* [한국어] NULL 포인터는 비활성 */
		return false;
	}

	/* Listener was stopped. */
	if (!listener->trid) {			/* [한국어] trid=NULL은 stop된 listener */
		return false;
	}

	return true;				/* [한국어] trid 유효 — 활성 listener */
}

/**
 * Function to be called once the target is listening.
 *
 * \param ctx Context argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
/*
 * [한국어]
 * _nvmf_subsystem_add_listener_done - transport listen_associate 콜백 완료 처리
 *
 * @ctx:    struct spdk_nvmf_subsystem_listener* (listener 포인터 직접 전달)
 * @status: 0=성공, 음수=에러 (transport listen_associate 실패)
 *
 * _nvmf_subsystem_add_listener의 마지막 단계. transport에서 listen_associate가
 * 완료(또는 에러)되면 이 함수가 호출된다. 성공 시 listener를 subsystem의 TAILQ에
 * 삽입하고, discovery subsystem이라면 mDNS PRR을 업데이트한 뒤 discovery log notice를
 * 발송한다. 실패(에러 또는 mDNS 업데이트 실패) 시 listener를 할당 해제한다.
 *
 * 호출 체인:
 *   _nvmf_subsystem_add_listener → transport->listen_associate → [_nvmf_subsystem_add_listener_done]
 *     → listener->cb_fn (완료 통지)
 */
static void
_nvmf_subsystem_add_listener_done(void *ctx, int status)
{
	struct spdk_nvmf_subsystem_listener *listener = ctx;	/* [한국어] 완료된 listener */
	struct spdk_nvmf_subsystem *subsystem = listener->subsystem;	/* [한국어] 대상 subsystem */

	if (status) {
		goto done;	/* [한국어] transport 에러 — 삽입 없이 done으로 */
	}

	TAILQ_INSERT_HEAD(&subsystem->listeners, listener, link);
	/* [한국어] 활성 listener 목록 선두에 삽입 (신규 connect부터 이 trid로 reach 가능) */

	if (spdk_nvmf_subsystem_is_discovery(subsystem)) {
		status = nvmf_tgt_update_mdns_prr(subsystem->tgt);
		/* [한국어] discovery subsystem의 listener 추가 시 mDNS PRR(Polling Resource Record) 업데이트 */
		if (status) {
			TAILQ_REMOVE(&subsystem->listeners, listener, link);
			/* [한국어] mDNS 업데이트 실패 — 방금 삽입한 listener 제거 (롤백) */
			goto done;
		}
	}

	SPDK_DTRACE_PROBE4(nvmf_subsystem_add_listener, subsystem->subnqn, listener->trid->trtype,
			   listener->trid->traddr, listener->trid->trsvcid);
	/* [한국어] DTrace 프로브: subsystem listener 추가 이벤트 기록 */

	spdk_nvmf_send_discovery_log_notice(subsystem->tgt, NULL);
	/* [한국어] 연결된 호스트들에게 discovery log 변경 AER(Asynchronous Event Request) 전송 */

done:
	listener->cb_fn(listener->cb_arg, status);	/* [한국어] 요청자에게 결과 통지 */
	if (status) {
		free(listener->ana_state);	/* [한국어] 에러 시 listener 정리 — ana_state 배열 해제 */
		free(listener->opts.sock_impl);	/* [한국어] socket 구현체 이름 문자열 해제 */
		free(listener);			/* [한국어] listener 구조체 자체 해제 */
	}
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_listener_opts_init - listener 옵션 구조체를 안전하게 기본값으로 초기화
 *
 * @opts: 초기화할 opts 구조체 포인터 (호출자가 할당)
 * @size: 호출자가 할당한 opts 구조체 크기 (하위 호환성을 위해 ABI versioning)
 *
 * opts_size 기반 FIELD_OK 매크로로 하위 버전 구조체에서 존재하지 않는 필드를
 * 안전하게 건너뜀 (API/ABI 변경 시에도 기존 바이너리 호환 유지).
 * 기본값: secure_channel=false, ana_state=OPTIMIZED, sock_impl=NULL.
 */
void
spdk_nvmf_subsystem_listener_opts_init(struct spdk_nvmf_listener_opts *opts, size_t size)
{
	if (opts == NULL) {			/* [한국어] NULL 포인터 방어 */
		SPDK_ERRLOG("opts should not be NULL\n");
		assert(false);
		return;
	}
	if (size == 0) {			/* [한국어] 크기 0 방어 (최소 1바이트) */
		SPDK_ERRLOG("size should not be zero\n");
		assert(false);
		return;
	}

	memset(opts, 0, size);			/* [한국어] 구조체 전체 0 초기화 */
	opts->opts_size = size;			/* [한국어] ABI 버전 힌트로 opts_size 기록 */

#define FIELD_OK(field) \
	offsetof(struct spdk_nvmf_listener_opts, field) + sizeof(opts->field) <= size
/* [한국어] 해당 필드가 호출자의 opts 구조체 크기 안에 있으면 접근 허용 */

#define SET_FIELD(field, value) \
	if (FIELD_OK(field)) { \
		opts->field = value; \
	} \
/* [한국어] FIELD_OK일 때만 기본값 설정 (이전 버전 ABI 호환) */

	SET_FIELD(secure_channel, false);		/* [한국어] TLS 비보안 채널 (기본) */
	SET_FIELD(ana_state, SPDK_NVME_ANA_OPTIMIZED_STATE);	/* [한국어] ANA 상태: OPTIMIZED (최적 경로) */
	SET_FIELD(sock_impl, NULL);			/* [한국어] socket 구현체: 기본 (NULL = default) */

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * listener_opts_copy - listener 옵션을 src에서 dst로 ABI-안전하게 복사
 *
 * @src: 복사 원본 opts (opts_size 기반으로 유효 필드 판별)
 * @dst: 복사 대상 opts (sizeof(*dst) 전체를 대상으로 함)
 * @return: 0=성공, -EINVAL(src->opts_size==0)
 *
 * src의 opts_size를 기반으로 src에 실제로 존재하는 필드만 dst에 복사.
 * 구조체가 버전업되어도 이전 버전의 src를 안전하게 처리.
 * SPDK_STATIC_ASSERT로 구조체 크기를 컴파일 시간에 검증 (신규 필드 추가 시 에러).
 */
static int
listener_opts_copy(struct spdk_nvmf_listener_opts *src, struct spdk_nvmf_listener_opts *dst)
{
	if (src->opts_size == 0) {			/* [한국어] opts_size 0은 초기화 안 된 구조체 */
		SPDK_ERRLOG("source structure size should not be zero\n");
		assert(false);
		return -EINVAL;
	}

	memset(dst, 0, sizeof(*dst));			/* [한국어] 대상 0 초기화 (미설정 필드는 0) */
	dst->opts_size = src->opts_size;		/* [한국어] opts_size 그대로 복사 */

#define FIELD_OK(field) \
	offsetof(struct spdk_nvmf_listener_opts, field) + sizeof(src->field) <= src->opts_size
/* [한국어] src opts_size 범위 내에 해당 필드가 있으면 복사 허용 */

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		dst->field = src->field; \
	} \
/* [한국어] FIELD_OK이면 src→dst 복사 */

	SET_FIELD(secure_channel);	/* [한국어] TLS 여부 복사 */
	SET_FIELD(ana_state);		/* [한국어] ANA 초기 상태 복사 */
	SET_FIELD(sock_impl);		/* [한국어] socket 구현체 이름 복사 (포인터만, 문자열 소유권 미이전) */
	/* We should not remove this statement, but need to update the assert statement
	 * if we add a new field, and also add a corresponding SET_FIELD statement. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_listener_opts) == 24, "Incorrect size");
	/* [한국어] 구조체 크기 컴파일 시간 검증 — 필드 추가 시 이 어서션도 업데이트 필요 */

#undef SET_FIELD
#undef FIELD_OK

	return 0;
}

/*
 * [한국어]
 * _nvmf_subsystem_add_listener - listener 추가 내부 구현 (opts 포함)
 *
 * @subsystem: 대상 subsystem (INACTIVE 또는 PAUSED 상태여야 함)
 * @trid:      추가할 transport ID
 * @cb_fn:     완료 콜백 (비동기이므로 NULL 불가)
 * @cb_arg:    콜백 인자
 * @opts:      listener 옵션 (NULL이면 기본값 사용)
 *
 * listener 추가의 전체 흐름:
 *   1) 상태 확인 (INACTIVE/PAUSED만 허용)
 *   2) 중복 check (이미 있으면 성공으로 no-op)
 *   3) trid의 transport 검색 (없으면 에러)
 *   4) transport 레벨 listener 검색 (없으면 에러)
 *   5) listener 구조체 할당 및 초기화 (ana_state 배열 포함)
 *   6) listener ID(bit array) 할당
 *   7) transport->listen_associate 호출
 *   8) _nvmf_subsystem_add_listener_done으로 결과 처리 (삽입 또는 해제)
 *
 * 실행 컨텍스트: 임의 스레드 (subsystem TAILQ 접근은 INACTIVE/PAUSED 상태 보호).
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_add_listener[_ext] → [_nvmf_subsystem_add_listener]
 *     → transport->listen_associate → _nvmf_subsystem_add_listener_done → cb_fn
 */
static void
_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
			     const struct spdk_nvme_transport_id *trid,
			     spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
			     void *cb_arg, struct spdk_nvmf_listener_opts *opts)
{
	struct spdk_nvmf_transport *transport;		/* [한국어] trid에 해당하는 transport */
	struct spdk_nvmf_subsystem_listener *listener;	/* [한국어] 새로 생성할 listener */
	struct spdk_nvmf_listener *tr_listener;		/* [한국어] transport 레벨 listener (trid 매핑) */
	uint32_t i;					/* [한국어] ana_state 초기화 루프 인덱스 */
	uint32_t id;					/* [한국어] bit array에서 할당받은 listener ID */
	int rc = 0;					/* [한국어] 에러 코드 */

	assert(cb_fn != NULL);				/* [한국어] 비동기 API — cb_fn 필수 */

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		/* [한국어] ACTIVE 상태에서는 listener 추가 금지 (상태 머신 일관성 보호) */
		cb_fn(cb_arg, -EAGAIN);
		return;
	}

	if (nvmf_subsystem_find_listener(subsystem, trid)) {
		/* Listener already exists in this subsystem */
		/* [한국어] 중복 listener — 이미 존재하면 성공 처리 (idempotent) */
		cb_fn(cb_arg, 0);
		return;
	}

	transport = spdk_nvmf_tgt_get_transport(subsystem->tgt, trid->trstring);
	/* [한국어] trstring(ex: "TCP", "RDMA")으로 이미 생성된 transport 검색 */
	if (!transport) {
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    trid->trstring);
		cb_fn(cb_arg, -EINVAL);
		return;		/* [한국어] transport가 없으면 listener 추가 불가 */
	}

	tr_listener = nvmf_transport_find_listener(transport, trid);
	/* [한국어] transport가 실제로 이 trid(traddr+trsvcid)에서 수신 중인지 확인 */
	if (!tr_listener) {
		SPDK_ERRLOG("Cannot find transport listener for %s\n", trid->traddr);
		cb_fn(cb_arg, -EINVAL);
		return;		/* [한국어] spdk_nvmf_tgt_listen이 먼저 호출되어야 함 */
	}

	listener = calloc(1, sizeof(*listener));	/* [한국어] 0 초기화 listener 구조체 할당 */
	if (!listener) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	listener->trid = &tr_listener->trid;		/* [한국어] transport listener의 trid 참조 (소유 아님) */
	listener->transport = transport;		/* [한국어] 소속 transport */
	listener->cb_fn = cb_fn;			/* [한국어] 완료 콜백 저장 */
	listener->cb_arg = cb_arg;			/* [한국어] 완료 콜백 인자 저장 */
	listener->subsystem = subsystem;		/* [한국어] 역참조 — done 콜백에서 subsystem 접근용 */
	listener->ana_state = calloc(subsystem->max_nsid, sizeof(enum spdk_nvme_ana_state));
	/* [한국어] NS별 ANA 상태 배열 할당 (인덱스 = nsid-1) */
	if (!listener->ana_state) {
		_nvmf_subsystem_add_listener_done(listener, -ENOMEM);
		/* [한국어] 메모리 부족 — done으로 에러 처리 (listener 해제 포함) */
		return;
	}

	spdk_nvmf_subsystem_listener_opts_init(&listener->opts, sizeof(listener->opts));
	/* [한국어] opts를 기본값으로 초기화 */
	if (opts != NULL) {
		rc = listener_opts_copy(opts, &listener->opts);
		/* [한국어] 호출자 opts를 listener->opts로 ABI-안전 복사 */
		if (rc) {
			SPDK_ERRLOG("Unable to copy listener options\n");
			_nvmf_subsystem_add_listener_done(listener, -EINVAL);
			return;
		}
	}

	id = spdk_bit_array_find_first_clear(subsystem->used_listener_ids, 0);
	/* [한국어] 사용 가능한 listener ID를 bit array에서 탐색 (선형 탐색, O(n)) */
	if (id == UINT32_MAX) {
		SPDK_ERRLOG("Cannot add any more listeners\n");
		_nvmf_subsystem_add_listener_done(listener, -EINVAL);
		return;		/* [한국어] bit array 포화 — 더 이상 listener 추가 불가 */
	}

	spdk_bit_array_set(subsystem->used_listener_ids, id);	/* [한국어] bit 점유 표시 */
	listener->id = id;					/* [한국어] listener에 ID 기록 */

	for (i = 0; i < subsystem->max_nsid; i++) {
		listener->ana_state[i] = listener->opts.ana_state;
		/* [한국어] 모든 NS의 초기 ANA 상태를 opts.ana_state로 설정 (기본: OPTIMIZED) */
	}

	if (transport->ops->listen_associate != NULL) {
		rc = transport->ops->listen_associate(transport, subsystem, trid);
		/* [한국어] transport-specific 리스너 연결 작업 (ex: TCP TLS PSK 등록) */
		if (rc) {
			SPDK_ERRLOG("Associate listener for transport %s failed with rc:%d\n", trid->trstring, rc);
		}
	}

	_nvmf_subsystem_add_listener_done(listener, rc);
	/* [한국어] 성공 시 TAILQ 삽입 + discovery notice, 실패 시 listener 해제 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_add_listener - subsystem을 transport 주소에 노출 (옵션 없는 버전)
 *
 * @subsystem: 노출할 subsystem (INACTIVE 또는 PAUSED 상태)
 * @trid:      transport id (trtype=TCP/RDMA/FC, traddr=IP/NN, trsvcid=port)
 * @cb_fn:     listener 등록 완료 콜백 (rc 0=성공)
 * @cb_arg:    콜백 인자
 *
 * 호스트는 trid 주소로 connect/discovery 요청 시 이 subsystem에 도달 가능.
 * 사전 조건: 이미 spdk_nvmf_tgt_listen으로 transport 자체가 trid 주소에서
 * 수신 중이어야 한다 (transport_listener와 subsystem_listener는 분리).
 * 비동기 — 실제 listen_associate transport 콜백이 cb_fn으로 결과 통지.
 */
void
spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				 const struct spdk_nvme_transport_id *trid,
				 spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				 void *cb_arg)
{
	_nvmf_subsystem_add_listener(subsystem, trid, cb_fn, cb_arg, NULL);
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_add_listener_ext - opts 인자가 있는 add_listener 확장판
 *
 * @opts: secure_channel(TLS), ana_state, sock_impl 등 listener-당 옵션
 * 그 외는 spdk_nvmf_subsystem_add_listener와 동일.
 */
void
spdk_nvmf_subsystem_add_listener_ext(struct spdk_nvmf_subsystem *subsystem,
				     const struct spdk_nvme_transport_id *trid,
				     spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				     void *cb_arg, struct spdk_nvmf_listener_opts *opts)
{
	_nvmf_subsystem_add_listener(subsystem, trid, cb_fn, cb_arg, opts);
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_remove_listener - 등록된 listener 제거
 *
 * @return: 0=성공, -EAGAIN(ACTIVE 상태), -ENOENT(없음)
 *
 * 해당 trid로의 신규 connect는 더 이상 이 subsystem에 도달하지 못한다.
 * 기존 ctrlr는 disconnect되지 않으나 ctrlr->listener 포인터는 NULL로 갱신.
 */
int
spdk_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
				    const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_subsystem_listener *listener;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return -EAGAIN;
	}

	listener = nvmf_subsystem_find_listener(subsystem, trid);
	if (listener == NULL) {
		return -ENOENT;
	}

	SPDK_DTRACE_PROBE4(nvmf_subsystem_remove_listener, subsystem->subnqn, listener->trid->trtype,
			   listener->trid->traddr, listener->trid->trsvcid);

	_nvmf_subsystem_remove_listener(subsystem, listener, false);

	return 0;
}

/*
 * [한국어]
 * nvmf_subsystem_remove_all_listeners - subsystem의 모든 listener 제거
 *
 * @subsystem: 대상 subsystem
 * @stop:      true=trid를 NULL로 설정(stop 표시), false=완전 제거
 *
 * 내부적으로 _nvmf_subsystem_remove_listener를 각 listener에 호출.
 * SAFE 순회 사용 — remove 중 TAILQ 변경 시 이터레이터 보호.
 * subsystem destroy 또는 deactivate 시 호출된다.
 */
void
nvmf_subsystem_remove_all_listeners(struct spdk_nvmf_subsystem *subsystem,
				    bool stop)
{
	struct spdk_nvmf_subsystem_listener *listener, *listener_tmp;
	/* [한국어] TAILQ_FOREACH_SAFE의 현재/임시 포인터 */

	TAILQ_FOREACH_SAFE(listener, &subsystem->listeners, link, listener_tmp) {
		/* [한국어] SAFE 버전 — remove 시 next 포인터 무효화 방지 */
		_nvmf_subsystem_remove_listener(subsystem, listener, stop);
		/* [한국어] 각 listener 제거 (stop=true이면 trid NULL로 stop 표시 후 메모리 해제) */
	}
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_listener_allowed - 이 subsystem이 특정 trid로 connect를 허용하는지 확인
 *
 * @subsystem: 대상 subsystem
 * @trid:      확인할 transport ID
 * @return: true=허용, false=거부
 *
 * 활성 listener 목록을 탐색해 trid 일치 여부 확인.
 * 레거시 호환: discovery NQN subsystem에 listener가 없어도 허용 (deprecated, 향후 제거 예정).
 */
bool
spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
				     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_subsystem_listener *listener;	/* [한국어] 탐색 중인 listener */

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (!nvmf_subsystem_listener_is_active(listener)) {
			continue;	/* [한국어] 비활성(stopped) listener 건너뜀 */
		}

		if (spdk_nvme_transport_id_compare(listener->trid, trid) == 0) {
			return true;	/* [한국어] trid 일치 — 허용 */
		}
	}

	if (!strcmp(subsystem->subnqn, SPDK_NVMF_DISCOVERY_NQN)) {
		SPDK_WARNLOG("Allowing connection to discovery subsystem on %s/%s/%s, "
			     "even though this listener was not added to the discovery "
			     "subsystem.  This behavior is deprecated and will be removed "
			     "in a future release.\n",
			     spdk_nvme_transport_id_trtype_str(trid->trtype), trid->traddr, trid->trsvcid);
		/* [한국어] 레거시 호환: discovery NQN subsystem은 listener 없이도 connect 허용 (deprecated) */
		return true;
	}

	return false;	/* [한국어] 목록에 없고 레거시 경로도 아님 — 거부 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_first_listener - subsystem의 첫 번째 listener 반환
 *
 * @subsystem: 조회할 subsystem
 * @return: 첫 번째 listener (비활성 포함), 없으면 NULL
 *
 * RPC get_subsystems 등에서 listener 목록 직렬화 시 사용.
 */
struct spdk_nvmf_subsystem_listener *
spdk_nvmf_subsystem_get_first_listener(struct spdk_nvmf_subsystem *subsystem)
{
	return TAILQ_FIRST(&subsystem->listeners);	/* [한국어] TAILQ 첫 번째 엔트리 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_next_listener - 다음 listener 반환
 *
 * @prev_listener: 현재 listener
 * @return: 다음 listener, 없으면 NULL
 */
struct spdk_nvmf_subsystem_listener *
spdk_nvmf_subsystem_get_next_listener(struct spdk_nvmf_subsystem *subsystem,
				      struct spdk_nvmf_subsystem_listener *prev_listener)
{
	return TAILQ_NEXT(prev_listener, link);		/* [한국어] TAILQ 다음 엔트리 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_listener_get_trid - listener의 transport ID 반환
 *
 * @listener: 대상 listener
 * @return: trid 포인터 (listener 수명 동안 유효)
 *
 * RPC, ctrlr 등에서 listener의 주소 정보 조회 시 사용.
 */
const struct spdk_nvme_transport_id *
spdk_nvmf_subsystem_listener_get_trid(struct spdk_nvmf_subsystem_listener *listener)
{
	return listener->trid;		/* [한국어] transport listener의 trid 직접 반환 (소유 아님) */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_allow_any_listener - subsystem의 allow_any_listener 플래그 설정
 *
 * @allow_any_listener: true이면 어느 listener로 들어온 connect도 이 subsystem에 허용
 *
 * listener ACL 없이 임의 trid로의 connect를 허용하는 플래그.
 * discovery subsystem에서 주로 사용.
 */
void
spdk_nvmf_subsystem_allow_any_listener(struct spdk_nvmf_subsystem *subsystem,
				       bool allow_any_listener)
{
	subsystem->flags.allow_any_listener = allow_any_listener;
	/* [한국어] 비트 플래그 flags.allow_any_listener 설정 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_any_listener_allowed - allow_any_listener 플래그 조회
 *
 * @return: true=모든 listener 허용, false=listener ACL 기반 허용
 */
bool
spdk_nvmf_subsystem_any_listener_allowed(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->flags.allow_any_listener;	/* [한국어] 플래그 직접 반환 */
}

/*
 * [한국어]
 * nvmf_subsystem_poll_group_update_ns_reservation - NS의 PR 상태를 poll group 캐시에 동기화
 *
 * @ns:    업데이트 원본 namespace (subsystem thread에서 실행, PR 상태 마스터)
 * @pg_ns: 업데이트 대상 poll group NS 캐시 (pg thread에서 IO 검증 시 사용)
 *
 * NVMe Persistent Reservation(PR)의 상태(crkey, rtype, holder, 등록자 목록)를
 * 각 poll group의 pg_ns 캐시에 스냅샷으로 복사한다.
 * poll group thread는 pg_ns 캐시를 읽어 IO의 reservation 접근 권한을 신속히 판별.
 * 실행 컨텍스트: subsystem thread(ns 변경 측) 또는 pg 업데이트 콜백에서 호출.
 *
 * 호출 체인:
 *   nvmf_ns_reservation_update_state → ns_reservation_pg_update → [nvmf_subsystem_poll_group_update_ns_reservation]
 */
void
nvmf_subsystem_poll_group_update_ns_reservation(const struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_subsystem_pg_ns_info *pg_ns)
{
	uint32_t j;				/* [한국어] 등록자 목록 인덱스 */
	struct spdk_nvmf_registrant *reg;	/* [한국어] 순회 중인 registrant */

	pg_ns->crkey = ns->crkey;		/* [한국어] Current Reservation Key (reservation holder 식별용) */
	pg_ns->rtype = ns->rtype;		/* [한국어] Reservation Type (EXCLUSIVE/WRITE_EXCLUSIVE 등) */
	if (ns->holder) {
		pg_ns->holder_id = ns->holder->hostid;
		/* [한국어] reservation holder의 hostid 복사 (IO 허용 판별 기준) */
	} else {
		memset(&pg_ns->holder_id, 0, sizeof(pg_ns->holder_id));
		/* [한국어] 현재 holder 없음 — holder_id 초기화 */
	}

	memset(&pg_ns->reg_hostid, 0, SPDK_NVMF_MAX_NUM_REGISTRANTS * sizeof(struct spdk_uuid));
	/* [한국어] 등록자 hostid 배열 초기화 (이전 스냅샷 삭제) */
	j = 0;
	TAILQ_FOREACH(reg, &ns->registrants, link) {
		/* [한국어] 모든 registrant를 pg_ns->reg_hostid 배열에 복사 */
		if (j >= SPDK_NVMF_MAX_NUM_REGISTRANTS) {
			SPDK_ERRLOG("Maximum %u registrants can support.\n", SPDK_NVMF_MAX_NUM_REGISTRANTS);
			/* This should never happen as we enforce SPDK_NVMF_MAX_NUM_REGISTRANTS
			 * on ns->registrants, but we don't want to continue with poll groups
			 * missing registrants.
			 */
			abort();	/* [한국어] 배열 초과는 버그 — abort로 디버깅 용이 */
		}
		pg_ns->reg_hostid[j++] = reg->hostid;	/* [한국어] hostid 복사 및 인덱스 증가 */
	}
}

/*
 * [한국어]
 * ns_reservation_hostid_list_contains_id - hostid 배열에 특정 hostid가 있는지 확인
 *
 * @hostid_list: 검색 대상 hostid UUID 배열
 * @num_hostid:  배열 내 유효 항목 수
 * @id:          찾을 hostid UUID
 * @return: true=배열 내 존재, false=없음
 *
 * preempt-and-abort 시 피해를 입은(preempted) hostid 목록에 특정 ctrlr의
 * hostid가 포함되어 있는지 빠르게 확인하는 선형 탐색.
 */
static bool
ns_reservation_hostid_list_contains_id(const struct spdk_uuid *hostid_list, uint32_t num_hostid,
				       const struct spdk_uuid *id)
{
	size_t i;	/* [한국어] 배열 탐색 인덱스 */

	for (i = 0; i < num_hostid; i++) {
		if (!spdk_uuid_compare(&hostid_list[i], id)) {
			return true;	/* [한국어] UUID 일치 — 존재 */
		}
	}
	return false;	/* [한국어] 배열 전체 탐색 후 미발견 */
}

/*
 * [한국어]
 * ns_reservation_io_should_wait - 이 IO 커맨드가 preempt-abort 완료를 기다려야 하는지 판단
 *
 * @cmd: 확인할 NVMe 커맨드
 * @return: true=대기 필요(일반 I/O), false=대기 불필요(reservation 상태 변경 커맨드)
 *
 * preempt-and-abort 진행 중에는 피해 호스트의 NS에 대한 I/O를 홀드시킨다.
 * 단, reservation 상태 변경 커맨드(REGISTER/ACQUIRE/RELEASE) 자체는 직렬화되어
 * 대기 큐에 넣으면 데드락이 발생하므로 예외 처리.
 */
static bool
ns_reservation_io_should_wait(const struct spdk_nvme_cmd *cmd)
{
	switch (cmd->opc) {
	/* We don't wait on reservation commands that modify state because
	 * those are serialized and will cause a deadlock.
	 */
	case SPDK_NVME_OPC_RESERVATION_REGISTER:	/* [한국어] REGISTER — 직렬화 대상, 대기 금지 */
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:		/* [한국어] ACQUIRE — 직렬화 대상, 대기 금지 */
	case SPDK_NVME_OPC_RESERVATION_RELEASE:		/* [한국어] RELEASE — 직렬화 대상, 대기 금지 */
		return false;
	default:
		return true;	/* [한국어] READ/WRITE 등 일반 I/O — preempt 완료까지 대기 필요 */
	}
}

/*
 * [한국어]
 * ns_reservation_req_is_preempt_abort - 요청이 PREEMPT AND ABORT인지 확인
 *
 * @req: 확인할 nvmf 요청
 * @return: true=RESERVATION_ACQUIRE 커맨드이고 RACQA=PREEMPT_ABORT
 *
 * NVMe spec: RESERVATION_ACQUIRE 커맨드에서 RACQA(Reservation Acquire Command Action) 필드로
 * acquire 동작을 구분. PREEMPT_ABORT(2)는 다른 호스트의 reservation을 강탈하고 그 호스트의
 * 모든 ctrlr를 abort한다.
 */
static bool
ns_reservation_req_is_preempt_abort(const struct spdk_nvmf_request *req)
{
	const struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;	/* [한국어] 요청 커맨드 구조체 */

	return cmd->opc == SPDK_NVME_OPC_RESERVATION_ACQUIRE &&
	       cmd->cdw10_bits.resv_acquire.racqa == SPDK_NVME_RESERVE_PREEMPT_ABORT;
	/* [한국어] ACQUIRE 커맨드이고 RACQA=PREEMPT_ABORT(2)이면 true */
}

/*
 * [한국어]
 * poll_group_reservation_build_io_waiting - preempt-abort 시 피해 호스트의 대기 중 IO 목록 구성
 *
 * @group:     현재 poll group
 * @subsystem: 대상 subsystem
 * @ns:        preempt-abort가 진행 중인 namespace
 * @req:       preempt-abort 요청 커맨드
 * @pg_ns:     업데이트할 poll group NS 캐시 (io_waiting 카운터 설정)
 *
 * ns->preempt_abort의 hostids 목록에 포함된 ctrlr(피해 호스트)의 qpair에서
 * 동일 NS에 대한 outstanding IO를 찾아 reservation_waiting 플래그를 설정하고
 * pg_ns->preempt_abort.io_waiting 카운터를 증가시킨다.
 * IO들이 완료될 때까지 preempt-abort 완료를 보류한다.
 *
 * 호출 체인:
 *   poll_group_reservation_preempt_abort_process → [poll_group_reservation_build_io_waiting]
 */
static void
poll_group_reservation_build_io_waiting(const struct spdk_nvmf_poll_group *group,
					const struct spdk_nvmf_subsystem *subsystem, const struct spdk_nvmf_ns *ns,
					const struct spdk_nvmf_request *req, struct spdk_nvmf_subsystem_pg_ns_info *pg_ns)
{
	struct spdk_nvmf_qpair *qpair;		/* [한국어] 순회 중인 qpair */
	struct spdk_nvmf_request *q_req;	/* [한국어] qpair의 outstanding 요청 */
	struct spdk_nvmf_reservation_preempt_abort_info *p_info = ns->preempt_abort;
	/* [한국어] preempt-abort 메타 정보 (피해 hostid 목록, gen) */
	const struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;	/* [한국어] preempt-abort 커맨드 */
	bool hostid_match;			/* [한국어] qpair ctrlr의 hostid가 피해 목록에 있는지 */

	pg_ns->preempt_abort.io_waiting = 0;	/* [한국어] 대기 IO 카운터 초기화 */
	if (!p_info->hostids_cnt) {
		/* no preempted hostids */
		return;		/* [한국어] 피해 hostid가 없으면 대기 IO 없음 */
	}
	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		/* [한국어] 이 poll group의 모든 qpair 순회 */
		if (!qpair->ctrlr || qpair->ctrlr->subsys != subsystem) {
			continue;	/* [한국어] ctrlr 없거나 다른 subsystem의 qpair 건너뜀 */
		}
		hostid_match = ns_reservation_hostid_list_contains_id(p_info->hostids,
				p_info->hostids_cnt, &qpair->ctrlr->hostid);
		/* [한국어] 이 ctrlr의 hostid가 피해 목록에 포함되어 있는지 확인 */
		if (!hostid_match) {
			continue;	/* [한국어] 피해 목록에 없는 ctrlr — 건너뜀 */
		}

		/* This is a preempted controller, check for IOs on the same namespace */
		TAILQ_FOREACH(q_req, &qpair->outstanding, link) {
			/* [한국어] 피해 ctrlr의 모든 outstanding 요청 검사 */
			struct spdk_nvme_cmd *req_cmd = &q_req->cmd->nvme_cmd;
			if (req_cmd->nsid == cmd->nsid && ns_reservation_io_should_wait(req_cmd)) {
				/* [한국어] 동일 NS에 대한 대기 가능 IO이면 waiting 표시 */
				pg_ns->preempt_abort.io_waiting++;	/* [한국어] 대기 IO 카운터 증가 */
				q_req->reservation_waiting = 1;		/* [한국어] 해당 요청에 대기 플래그 설정 */
			}
		}
	}
}

/*
 * [한국어]
 * poll_group_reservation_preempt_abort_process - poll group에서 preempt-abort 처리 시작
 *
 * @group:  현재 poll group
 * @ns:     preempt-abort 대상 namespace
 * @pg_ns:  poll group NS 캐시
 *
 * NS의 reservation 큐에 PREEMPT_ABORT 요청이 있으면 이 poll group에서
 * 해당 호스트의 대기 중 IO를 찾아 io_waiting 카운터를 설정한다.
 * gen 기반 중복 처리 방지 (같은 gen의 요청은 이미 처리된 것으로 간주).
 *
 * 호출 체인:
 *   ns_reservation_pg_update → [poll_group_reservation_preempt_abort_process]
 *     → poll_group_reservation_build_io_waiting
 */
static void
poll_group_reservation_preempt_abort_process(struct spdk_nvmf_poll_group *group,
		struct spdk_nvmf_ns *ns, struct spdk_nvmf_subsystem_pg_ns_info *pg_ns)
{
	struct spdk_nvmf_request *req;	/* [한국어] reservation 큐의 첫 번째 요청 */

	/* Check for in-progress reservations to process */
	if (STAILQ_EMPTY(&ns->reservations)) {
		return;		/* [한국어] 처리 대기 중인 reservation 커맨드 없음 */
	}
	req = STAILQ_FIRST(&ns->reservations);	/* [한국어] 큐 선두 요청 (직렬화 처리 중) */
	/* Check if this is a preempt-and-abort cmd */
	if (!ns_reservation_req_is_preempt_abort(req)) {
		return;		/* [한국어] PREEMPT_ABORT가 아닌 다른 reservation 커맨드 — 처리 스킵 */
	}

	/* Ensure we have not already processed this */
	if (ns->preempt_abort->hostids_gen == pg_ns->preempt_abort.hostids_gen) {
		SPDK_ERRLOG("Poll group: %p already processed preempt hostids: %u\n",
			    group, ns->preempt_abort->hostids_gen);
		return;		/* [한국어] 이미 같은 gen을 처리함 — 중복 방지 */
	}

	if (pg_ns->preempt_abort.io_waiting) {
		/* This could happen if a previous preempt-and-abort failed before
		 * completing the IO waiting. Don't let this block the next abort
		 */
		SPDK_ERRLOG("Poll group: %p has incomplete preempted io waiting: %lu\n",
			    group, pg_ns->preempt_abort.io_waiting);
		/* [한국어] 이전 preempt-abort의 잔여 io_waiting — 덮어쓰기 (새 abort 진행 허용) */
	}

	poll_group_reservation_build_io_waiting(group, ns->subsystem, ns, req, pg_ns);
	/* [한국어] 피해 호스트의 대기 IO를 찾아 io_waiting 카운터 설정 */
	/* Commit gen as processed */
	pg_ns->preempt_abort.hostids_gen = ns->preempt_abort->hostids_gen;
	/* [한국어] 처리된 gen 기록 — 같은 gen의 중복 처리 방지 */
}

static void _nvmf_ns_reservation_update_done(struct spdk_nvmf_subsystem *subsystem,
		void *cb_arg, int status);
/* [한국어] 전방 선언 — ns_reservation_pg_update_done에서 호출하기 위함 */

/*
 * [한국어]
 * ns_reservation_pg_update_done - 모든 poll group의 PR 상태 업데이트 완료 처리
 *
 * @i:      spdk_for_each_channel 이터레이터 (ctx=ns 포인터)
 * @status: 0=성공 (에러 경로가 제거됨 — 비정상이면 abort)
 *
 * 모든 poll group에서 ns_reservation_pg_update가 완료된 후 이 함수가 호출된다.
 * 에러 발생 시 abort (poll group의 PR 상태 불일치는 데이터 손상 위험).
 * 성공 시 reservation 처리 대기 큐의 선두 요청을 _nvmf_ns_reservation_update_done으로 완료.
 *
 * 호출 체인:
 *   spdk_for_each_channel(complete) → [ns_reservation_pg_update_done]
 *     → _nvmf_ns_reservation_update_done
 */
static void
ns_reservation_pg_update_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_ns *ns = (struct spdk_nvmf_ns *)spdk_io_channel_iter_get_ctx(i);
	/* [한국어] ctx로부터 ns 포인터 추출 */

	if (status) {
		SPDK_ERRLOG("Poll group reservation updated failed on subsystem: %p, ns: %u\n",
			    ns->subsystem, ns->nsid);
		/*
		 * Errors paths have been eliminated for this poll group update, so
		 * this should never happen but if it does, that means the poll group
		 * reservation state is inconsistent and it's not safe to continue!!
		 */
		abort();	/* [한국어] PR 상태 불일치는 즉시 abort — 데이터 정합성 보장 불가 */
	}

	_nvmf_ns_reservation_update_done(ns->subsystem,
					 STAILQ_FIRST(&ns->reservations), 0);
	/* [한국어] reservation 큐 선두 요청을 완료 처리 (성공 응답 발송, 큐 dequeue) */
}

/*
 * [한국어]
 * ns_reservation_pg_update - 각 poll group에서 NS PR 상태 캐시 업데이트
 *
 * @i: spdk_for_each_channel 이터레이터 (ctx=ns 포인터, channel=poll group)
 *
 * 각 poll group의 pg_ns 캐시를 현재 NS의 PR 상태로 갱신한다.
 * 또한 preempt-abort 요청이 있으면 피해 호스트의 IO를 홀드 처리한다.
 * 모든 poll group 처리 후 ns_reservation_pg_update_done 호출.
 *
 * 실행 컨텍스트: 각 poll group thread.
 *
 * 호출 체인:
 *   nvmf_ns_reservation_update_state → spdk_for_each_channel → [ns_reservation_pg_update]
 *     → spdk_for_each_channel_continue → ns_reservation_pg_update_done
 */
static void
ns_reservation_pg_update(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_ns *ns;				/* [한국어] 업데이트할 NS */
	struct spdk_nvmf_poll_group *group;			/* [한국어] 현재 poll group */
	struct spdk_nvmf_subsystem_poll_group *sgroup;		/* [한국어] subsystem-specific poll group 정보 */
	struct spdk_nvmf_subsystem_pg_ns_info *pg_ns;		/* [한국어] poll group의 NS별 캐시 */

	ns = spdk_io_channel_iter_get_ctx(i);			/* [한국어] ctx에서 ns 추출 */
	group = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));
	/* [한국어] 현재 io_channel로부터 poll group 획득 */
	sgroup = &group->sgroups[ns->subsystem->id];		/* [한국어] subsystem ID로 subsystem poll group 인덱싱 */
	pg_ns = &sgroup->ns_info[ns->nsid - 1];		/* [한국어] nsid-1 인덱스로 pg_ns 접근 */

	nvmf_subsystem_poll_group_update_ns_reservation(ns, pg_ns);
	/* [한국어] NS의 PR 상태(crkey, rtype, holder, registrants)를 pg_ns 캐시에 스냅샷 */
	poll_group_reservation_preempt_abort_process(group, ns, pg_ns);
	/* [한국어] preempt-abort 요청이 있으면 피해 호스트 IO 홀드 처리 */

	spdk_for_each_channel_continue(i, 0);			/* [한국어] 이 poll group 완료, 다음 poll group으로 */
}

/*
 * [한국어]
 * nvmf_subsystem_ns_changed - NS 변경을 subsystem의 모든 ctrlr에 통지
 *
 * @subsystem: 대상 subsystem
 * @nsid:      변경된 NSID
 *
 * NS가 추가/제거/변경되면 NS를 볼 수 있는(visible) 모든 ctrlr에게
 * nvmf_ctrlr_ns_changed를 호출하여 Namespace Change (NS 변경) AER를 발송한다.
 * 이를 통해 호스트가 Identify Namespace Active List를 재조회하도록 유도.
 */
static void
nvmf_subsystem_ns_changed(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_ctrlr *ctrlr;		/* [한국어] 순회 중인 ctrlr */

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		/* [한국어] subsystem의 모든 ctrlr 순회 */
		if (nvmf_ctrlr_ns_is_visible(ctrlr, nsid)) {
			nvmf_ctrlr_ns_changed(ctrlr, nsid);
			/* [한국어] NS가 이 ctrlr에 visible이면 NS 변경 통지 (AER 발송) */
		}
	}
}

static uint32_t nvmf_ns_reservation_clear_all_registrants(struct spdk_nvmf_ns *ns);

/*
 * [한국어]
 * spdk_nvmf_subsystem_remove_ns - subsystem에서 NS 제거
 *
 * @subsystem: 대상 (INACTIVE 또는 PAUSED 필요)
 * @nsid:      제거할 NSID
 * @return: 0=성공, -1=상태 불일치/NSID 부적합/NS 없음
 *
 * 다음을 수행:
 *   1) NS의 host ACL 모두 제거
 *   2) PR 등록자 정리, ptpl_file/preempt_abort 메모리 해제
 *   3) bdev claim release + close
 *   4) ANA group 카운터 감소
 *   5) 각 transport에 NS 제거 통지 (ns_remove 콜백)
 *   6) 모든 ctrlr에게 NS invisible로 표시 (Identify 응답에 빠짐)
 *
 * 호출 컨텍스트: subsystem->thread. ACTIVE 상태에선 nvmf_ns_hot_remove를
 * 거쳐 pause 후 호출되어야 한다 (예: SPDK_BDEV_EVENT_REMOVE 이벤트).
 */
int
spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_host *host, *tmp;
	struct spdk_nvmf_ctrlr *ctrlr;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		assert(false);
		return -1;
	}

	if (nsid == 0 || nsid > subsystem->max_nsid) {
		return -1;
	}

	ns = subsystem->ns[nsid - 1];
	if (!ns) {
		return -1;
	}

	subsystem->ns[nsid - 1] = NULL;

	assert(ns->anagrpid - 1 < subsystem->max_nsid);
	assert(subsystem->ana_group[ns->anagrpid - 1] > 0);
	assert(STAILQ_EMPTY(&ns->reservations));

	subsystem->ana_group[ns->anagrpid - 1]--;

	TAILQ_FOREACH_SAFE(host, &ns->hosts, link, tmp) {
		nvmf_ns_remove_host(ns, host);
	}

	free(ns->ptpl_file);
	free(ns->preempt_abort);
	nvmf_ns_reservation_clear_all_registrants(ns);
	spdk_bdev_module_release_bdev(ns->bdev);
	spdk_bdev_close(ns->desc);
	free(ns);

	if (subsystem->fdp_supported && !spdk_nvmf_subsystem_get_first_ns(subsystem)) {
		subsystem->fdp_supported = false;
		SPDK_DEBUGLOG(nvmf, "Subsystem with id: %u doesn't have FDP capability.\n",
			      subsystem->id);
	}

	for (transport = spdk_nvmf_transport_get_first(subsystem->tgt); transport;
	     transport = spdk_nvmf_transport_get_next(transport)) {
		if (transport->ops->subsystem_remove_ns) {
			transport->ops->subsystem_remove_ns(transport, subsystem, nsid);
		}
	}

	nvmf_subsystem_ns_changed(subsystem, nsid);

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		nvmf_ctrlr_ns_set_visible(ctrlr, nsid, false);
	}

	return 0;
}

struct subsystem_ns_change_ctx {
	struct spdk_nvmf_subsystem		*subsystem;
	/* [한국어] NS 변경(제거/리사이즈) 대상 subsystem.
	 * 설정자: nvmf_ns_hot_remove / nvmf_ns_resize에서 초기화.
	 * 읽는 자: _nvmf_ns_hot_remove / _nvmf_ns_resize에서 subsystem_resume 호출 시.
	 * 값 범위: 유효한 subsystem 포인터 (비동기 완료까지 유효해야 함).
	 * 동기화: 비동기 pause/resume 시퀀스 내 단일 스레드 접근. */

	spdk_nvmf_subsystem_state_change_done	cb_fn;
	/* [한국어] pause 완료 후 호출할 콜백 (NS 제거 또는 리사이즈 수행).
	 * 설정자: nvmf_ns_hot_remove → _nvmf_ns_hot_remove,
	 *          nvmf_ns_resize → _nvmf_ns_resize.
	 * 읽는 자: nvmf_ns_change_msg에서 spdk_nvmf_subsystem_pause에 cb_fn으로 전달.
	 * 값 범위: 유효한 함수 포인터.
	 * 동기화: pause 완료 시 단일 호출, 경쟁 없음. */

	uint32_t				nsid;
	/* [한국어] 변경 대상 NS의 NSID.
	 * 설정자: nvmf_ns_hot_remove / nvmf_ns_resize에서 ns->opts.nsid로 초기화.
	 * 읽는 자: _nvmf_ns_hot_remove에서 spdk_nvmf_subsystem_remove_ns 인자로,
	 *          nvmf_subsystem_ns_changed에서 AER 발송 대상으로.
	 * 값 범위: 1~max_nsid (hot_remove 시) 또는 0 (resize 시 전체 quiesce 불필요).
	 * 동기화: 비동기 완료까지 불변 (읽기 전용). */
};

/*
 * [한국어]
 * _nvmf_ns_hot_remove - subsystem pause 완료 후 NS 제거 수행
 *
 * @subsystem: pause된 subsystem
 * @cb_arg:    subsystem_ns_change_ctx 포인터
 * @status:    pause 결과 (0=성공)
 *
 * spdk_nvmf_subsystem_pause의 콜백. subsystem이 PAUSED 상태가 되면
 * NS를 실제로 제거하고 resume한다. NS 제거 실패는 로그만 남기고 계속 진행
 * (resume은 반드시 해야 다음 상태 전환이 가능).
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause(complete) → [_nvmf_ns_hot_remove]
 *     → spdk_nvmf_subsystem_remove_ns → spdk_nvmf_subsystem_resume
 */
static void
_nvmf_ns_hot_remove(struct spdk_nvmf_subsystem *subsystem,
		    void *cb_arg, int status)
{
	struct subsystem_ns_change_ctx *ctx = cb_arg;	/* [한국어] NS 제거 컨텍스트 */
	int rc;						/* [한국어] 에러 코드 */

	rc = spdk_nvmf_subsystem_remove_ns(subsystem, ctx->nsid);
	/* [한국어] NS 제거 (bdev close, PR 정리, ctrlr에 invisible 통지 포함) */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to make changes to NVME-oF subsystem with id: %u\n", subsystem->id);
	}

	rc = spdk_nvmf_subsystem_resume(subsystem, NULL, NULL);
	/* [한국어] subsystem 다시 ACTIVE로 전환 (IO 수용 재개) */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to resume NVME-oF subsystem with id: %u\n", subsystem->id);
	}

	free(ctx);	/* [한국어] 비동기 ctx 해제 */
}

/*
 * [한국어]
 * nvmf_ns_change_msg - subsystem pause 재시도 메시지 핸들러
 *
 * @ns_ctx: subsystem_ns_change_ctx 포인터
 *
 * spdk_nvmf_subsystem_pause가 -EBUSY를 반환할 때 (다른 상태 전환 중)
 * 현재 스레드의 메시지 큐에 자기 자신을 재전송하여 재시도한다.
 * EBUSY가 아닌 에러는 ctx를 해제하고 포기.
 */
static void
nvmf_ns_change_msg(void *ns_ctx)
{
	struct subsystem_ns_change_ctx *ctx = ns_ctx;	/* [한국어] 재시도할 ctx */
	int rc;						/* [한국어] pause 반환 코드 */

	SPDK_DTRACE_PROBE2(nvmf_ns_change, ctx->nsid, ctx->subsystem->subnqn);
	/* [한국어] DTrace 프로브: NS 변경 재시도 이벤트 기록 */

	rc = spdk_nvmf_subsystem_pause(ctx->subsystem, ctx->nsid, ctx->cb_fn, ctx);
	/* [한국어] pause 재시도 */
	if (rc) {
		if (rc == -EBUSY) {
			/* Try again, this is not a permanent situation. */
			spdk_thread_send_msg(spdk_get_thread(), nvmf_ns_change_msg, ctx);
			/* [한국어] 현재 스레드에 재전송 — 다음 poll cycle에서 다시 시도 */
		} else {
			free(ctx);	/* [한국어] 영구적 에러 — ctx 해제 */
			SPDK_ERRLOG("Unable to pause subsystem to process namespace removal!\n");
		}
	}
}

/*
 * [한국어]
 * nvmf_ns_hot_remove - bdev REMOVE 이벤트 수신 후 NS 비동기 제거 시작
 *
 * @remove_ctx: spdk_nvmf_ns 포인터 (bdev_open_ext의 event_ctx)
 *
 * SPDK_BDEV_EVENT_REMOVE 이벤트(bdev가 사라짐)를 받으면 subsystem을
 * pause하고 NS를 제거한다. 비동기 op이므로 ctx를 별도 할당.
 * 이미 다른 상태 전환 중이면 nvmf_ns_change_msg로 재시도.
 *
 * 호출 체인:
 *   nvmf_ns_event(REMOVE) → [nvmf_ns_hot_remove]
 *     → spdk_nvmf_subsystem_pause → _nvmf_ns_hot_remove → remove_ns → resume
 */
static void
nvmf_ns_hot_remove(void *remove_ctx)
{
	struct spdk_nvmf_ns *ns = remove_ctx;		/* [한국어] 제거할 NS */
	struct subsystem_ns_change_ctx *ns_ctx;		/* [한국어] 비동기 작업용 ctx */
	int rc;						/* [한국어] pause 반환 코드 */

	/* We have to allocate a new context because this op
	 * is asynchronous and we could lose the ns in the middle.
	 */
	ns_ctx = calloc(1, sizeof(struct subsystem_ns_change_ctx));
	/* [한국어] 0 초기화 ctx 할당 — ns 포인터는 pause 완료 전에 무효화될 수 있으므로 별도 ctx */
	if (!ns_ctx) {
		SPDK_ERRLOG("Unable to allocate context to process namespace removal!\n");
		return;
	}

	ns_ctx->subsystem = ns->subsystem;		/* [한국어] 대상 subsystem */
	ns_ctx->nsid = ns->opts.nsid;			/* [한국어] 제거할 NSID */
	ns_ctx->cb_fn = _nvmf_ns_hot_remove;		/* [한국어] pause 완료 시 실행할 콜백 */

	rc = spdk_nvmf_subsystem_pause(ns->subsystem, ns_ctx->nsid, _nvmf_ns_hot_remove, ns_ctx);
	/* [한국어] 이 NSID의 IO를 quiesce하고 PAUSED 상태로 전환 */
	if (rc) {
		if (rc == -EBUSY) {
			/* Try again, this is not a permanent situation. */
			spdk_thread_send_msg(spdk_get_thread(), nvmf_ns_change_msg, ns_ctx);
			/* [한국어] EBUSY: 다른 상태 전환 중 — 메시지 큐로 재시도 */
		} else {
			SPDK_ERRLOG("Unable to pause subsystem to process namespace removal!\n");
			free(ns_ctx);	/* [한국어] 영구적 에러 — ctx 해제 */
		}
	}
}

/*
 * [한국어]
 * _nvmf_ns_resize - subsystem pause 완료 후 NS 리사이즈 처리
 *
 * @subsystem: pause된 subsystem
 * @cb_arg:    subsystem_ns_change_ctx 포인터
 * @status:    pause 결과
 *
 * NS 크기 변경(리사이즈)을 모든 ctrlr에 통지하고 subsystem을 resume한다.
 * 리사이즈는 항상 확대 방향이므로 IO quiesce(nsid 지정)는 불필요.
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_pause(complete) → [_nvmf_ns_resize]
 *     → nvmf_subsystem_ns_changed → spdk_nvmf_subsystem_resume
 */
static void
_nvmf_ns_resize(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	struct subsystem_ns_change_ctx *ctx = cb_arg;	/* [한국어] 리사이즈 ctx */

	nvmf_subsystem_ns_changed(subsystem, ctx->nsid);
	/* [한국어] 모든 ctrlr에 NS 변경 AER 발송 (호스트에게 리사이즈 통지) */
	if (spdk_nvmf_subsystem_resume(subsystem, NULL, NULL) != 0) {
		SPDK_ERRLOG("Failed to resume NVME-oF subsystem with id: %u\n", subsystem->id);
	}

	free(ctx);	/* [한국어] 비동기 ctx 해제 */
}

/*
 * [한국어]
 * nvmf_ns_resize - bdev RESIZE 이벤트 수신 후 NS 리사이즈 비동기 처리 시작
 *
 * @event_ctx: spdk_nvmf_ns 포인터 (bdev_open_ext의 event_ctx)
 *
 * SPDK_BDEV_EVENT_RESIZE 이벤트를 받으면 subsystem을 pause한다.
 * 리사이즈는 항상 확대 방향 → IO quiesce 불필요 → nsid=0으로 pause 호출.
 *
 * 호출 체인:
 *   nvmf_ns_event(RESIZE) → [nvmf_ns_resize]
 *     → spdk_nvmf_subsystem_pause(nsid=0) → _nvmf_ns_resize → ns_changed → resume
 */
static void
nvmf_ns_resize(void *event_ctx)
{
	struct spdk_nvmf_ns *ns = event_ctx;		/* [한국어] 리사이즈된 NS */
	struct subsystem_ns_change_ctx *ns_ctx;		/* [한국어] 비동기 ctx */
	int rc;						/* [한국어] pause 반환 코드 */

	/* We have to allocate a new context because this op
	 * is asynchronous and we could lose the ns in the middle.
	 */
	ns_ctx = calloc(1, sizeof(struct subsystem_ns_change_ctx));
	/* [한국어] 비동기 ctx 할당 */
	if (!ns_ctx) {
		SPDK_ERRLOG("Unable to allocate context to process namespace removal!\n");
		return;
	}

	ns_ctx->subsystem = ns->subsystem;		/* [한국어] 대상 subsystem */
	ns_ctx->nsid = ns->opts.nsid;			/* [한국어] 리사이즈된 NSID (ns_changed 통지용) */
	ns_ctx->cb_fn = _nvmf_ns_resize;		/* [한국어] pause 완료 시 실행할 콜백 */

	/* Specify 0 for the nsid here, because we do not need to pause the namespace.
	 * Namespaces can only be resized bigger, so there is no need to quiesce I/O.
	 */
	rc = spdk_nvmf_subsystem_pause(ns->subsystem, 0, _nvmf_ns_resize, ns_ctx);
	/* [한국어] nsid=0: 특정 NS IO quiesce 없이 subsystem 전체 pause (리사이즈는 항상 확대) */
	if (rc) {
		if (rc == -EBUSY) {
			/* Try again, this is not a permanent situation. */
			spdk_thread_send_msg(spdk_get_thread(), nvmf_ns_change_msg, ns_ctx);
			/* [한국어] EBUSY: 재시도 */
		} else {
			SPDK_ERRLOG("Unable to pause subsystem to process namespace resize!\n");
			free(ns_ctx);	/* [한국어] 영구적 에러 — ctx 해제 */
		}
	}
}

/*
 * [한국어]
 * nvmf_ns_event - bdev 이벤트 핸들러 (bdev_open_ext에 등록한 콜백)
 *
 * @type:      bdev 이벤트 타입 (REMOVE/RESIZE 등)
 * @bdev:      이벤트를 발생시킨 bdev
 * @event_ctx: spdk_nvmf_ns 포인터 (bdev_open_ext 시 등록된 컨텍스트)
 *
 * bdev 레이어에서 이 NS의 bdev에 이벤트가 발생하면 호출된다.
 * REMOVE: 기반 bdev가 사라짐 → NS 비동기 제거 (hot-remove)
 * RESIZE: bdev 용량 증가 → 호스트에 NS 변경 AER 발송
 *
 * 호출 체인:
 *   bdev_module → [nvmf_ns_event] → nvmf_ns_hot_remove | nvmf_ns_resize
 */
static void
nvmf_ns_event(enum spdk_bdev_event_type type,
	      struct spdk_bdev *bdev,
	      void *event_ctx)
{
	SPDK_DEBUGLOG(nvmf, "Bdev event: type %d, name %s, subsystem_id %d, ns_id %d\n",
		      type,
		      spdk_bdev_get_name(bdev),
		      ((struct spdk_nvmf_ns *)event_ctx)->subsystem->id,
		      ((struct spdk_nvmf_ns *)event_ctx)->nsid);
	/* [한국어] 디버그 로그: 이벤트 타입, bdev 이름, subsystem/NS ID */

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:		/* [한국어] bdev 제거 이벤트 — NS hot-remove */
		nvmf_ns_hot_remove(event_ctx);
		break;
	case SPDK_BDEV_EVENT_RESIZE:		/* [한국어] bdev 리사이즈 이벤트 — NS 크기 변경 통지 */
		nvmf_ns_resize(event_ctx);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;	/* [한국어] 지원하지 않는 이벤트 타입 — 무시 */
	}
}

/*
 * [한국어]
 * spdk_nvmf_ns_opts_get_defaults - NS opts 구조체를 기본값으로 초기화
 *
 * @opts:      초기화할 opts 구조체
 * @opts_size: 호출자가 할당한 opts 크기 (ABI versioning)
 *
 * spdk_nvmf_subsystem_add_ns_ext 호출 전 opts 구조체를 초기화.
 * nsid=0(자동 할당), anagrpid=0(nsid와 동일), uuid=NULL 등 기본값 설정.
 */
void
spdk_nvmf_ns_opts_get_defaults(struct spdk_nvmf_ns_opts *opts, size_t opts_size)
{
	if (!opts) {				/* [한국어] NULL 방어 */
		SPDK_ERRLOG("opts should not be NULL.\n");
		return;
	}

	if (!opts_size) {			/* [한국어] 크기 0 방어 */
		SPDK_ERRLOG("opts_size should not be zero.\n");
		return;
	}

	memset(opts, 0, opts_size);		/* [한국어] 구조체 전체 0 초기화 */
	opts->opts_size = opts_size;		/* [한국어] ABI 버전 힌트 기록 */

#define FIELD_OK(field) \
	offsetof(struct spdk_nvmf_ns_opts, field) + sizeof(opts->field) <= opts_size
/* [한국어] 해당 필드가 opts_size 내에 있으면 접근 허용 */

#define SET_FIELD(field, value) \
	if (FIELD_OK(field)) { \
		opts->field = value; \
	} \
/* [한국어] FIELD_OK이면 기본값 설정 */

	/* All current fields are set to 0 by default. */
	SET_FIELD(nsid, 0);			/* [한국어] 0=자동 NSID 할당 */
	if (FIELD_OK(nguid)) {
		memset(opts->nguid, 0, sizeof(opts->nguid));	/* [한국어] NGUID(Namespace GUID)=NULL */
	}
	if (FIELD_OK(eui64)) {
		memset(opts->eui64, 0, sizeof(opts->eui64));	/* [한국어] EUI-64 식별자=NULL */
	}
	if (FIELD_OK(uuid)) {
		spdk_uuid_set_null(&opts->uuid);	/* [한국어] UUID=NULL (bdev UUID 사용 안 함) */
	}
	SET_FIELD(anagrpid, 0);			/* [한국어] 0=NSID와 동일한 ANA group ID */
	SET_FIELD(transport_specific, NULL);	/* [한국어] transport별 추가 옵션 없음 */
	SET_FIELD(hide_metadata, false);	/* [한국어] DIF/DIX metadata 숨김 없음 */

#undef FIELD_OK
#undef SET_FIELD
}

/*
 * [한국어]
 * nvmf_ns_opts_copy - user_opts을 내부 opts로 ABI-안전하게 복사
 *
 * @opts:      복사 대상 (내부 기본값으로 이미 초기화되어 있어야 함)
 * @user_opts: 복사 원본 (user_opts->opts_size 범위 내 필드만 복사)
 * @opts_size: 미사용 (user_opts->opts_size로 범위 결정)
 *
 * user_opts->opts_size 범위 내 필드만 복사하여 이전 ABI 호환 유지.
 */
static void
nvmf_ns_opts_copy(struct spdk_nvmf_ns_opts *opts,
		  const struct spdk_nvmf_ns_opts *user_opts,
		  size_t opts_size)
{
#define FIELD_OK(field)	\
	offsetof(struct spdk_nvmf_ns_opts, field) + sizeof(opts->field) <= user_opts->opts_size
/* [한국어] user_opts->opts_size 범위 안에 있는 필드만 복사 허용 */

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		opts->field = user_opts->field;	\
	} \
/* [한국어] FIELD_OK이면 user_opts→opts 복사 */

	SET_FIELD(nsid);	/* [한국어] NSID 복사 (0=자동 할당) */
	if (FIELD_OK(nguid)) {
		memcpy(opts->nguid, user_opts->nguid, sizeof(opts->nguid));
		/* [한국어] NGUID(16바이트 GUID) 복사 */
	}
	if (FIELD_OK(eui64)) {
		memcpy(opts->eui64, user_opts->eui64, sizeof(opts->eui64));
		/* [한국어] EUI-64(8바이트 식별자) 복사 */
	}
	if (FIELD_OK(uuid)) {
		spdk_uuid_copy(&opts->uuid, &user_opts->uuid);
		/* [한국어] UUID 복사 (NVMe Identify Namespace에 보고됨) */
	}
	SET_FIELD(anagrpid);		/* [한국어] ANA Group ID 복사 */
	SET_FIELD(no_auto_visible);	/* [한국어] NS 자동 노출 억제 플래그 복사 */
	SET_FIELD(transport_specific);	/* [한국어] transport 특화 옵션 포인터 복사 */
	SET_FIELD(hide_metadata);	/* [한국어] metadata 숨김 플래그 복사 */

	opts->opts_size = user_opts->opts_size;	/* [한국어] opts_size 동기화 */

	/* We should not remove this statement, but need to update the assert statement
	 * if we add a new field, and also add a corresponding SET_FIELD statement.
	 */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ns_opts) == 73, "Incorrect size");
	/* [한국어] 구조체 크기 컴파일 시간 검증 — 필드 추가 시 이 어서션도 업데이트 필요 */

#undef FIELD_OK
#undef SET_FIELD
}

/* Dummy bdev module used to to claim bdevs. */
/* [한국어] NS에 할당된 bdev를 claim(독점)하기 위한 더미 bdev 모듈.
 * bdev_module_claim_bdev로 이 모듈 명의로 claim하여 다른 모듈이 같은 bdev를 NS로 추가하는 것을 방지. */
static struct spdk_bdev_module ns_bdev_module = {
	.name	= "NVMe-oF Target",
};

static int nvmf_ns_reservation_update(const struct spdk_nvmf_ns *ns,
				      const struct spdk_nvmf_reservation_info *info);
/* [한국어] 전방 선언 — NS의 PR 상태를 PTPL 파일에 저장 */
static int nvmf_ns_reservation_load(const struct spdk_nvmf_ns *ns,
				    struct spdk_nvmf_reservation_info *info);
/* [한국어] 전방 선언 — PTPL 파일에서 PR 상태 로드 */
static int nvmf_ns_reservation_restore(struct spdk_nvmf_ns *ns,
				       struct spdk_nvmf_reservation_info *info);
/* [한국어] 전방 선언 — 로드된 PR 상태를 NS에 복원 */

/*
 * [한국어]
 * nvmf_subsystem_zone_append_supported - subsystem에 zone append를 지원하는 NS가 있는지 확인
 *
 * @subsystem: 확인할 subsystem
 * @return: true=하나 이상의 NS가 zone bdev이고 zone_append IO 타입 지원
 *
 * ZNS(Zoned Namespace Storage) 지원 여부 확인. Zone append는 zoned bdev에서
 * write zone pointer 위치에 자동으로 데이터를 append하는 NVMe 2.0 기능.
 */
bool
nvmf_subsystem_zone_append_supported(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_ns *ns;	/* [한국어] 순회 중인 NS */

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem);
	     ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (spdk_bdev_is_zoned(ns->bdev) &&
		    spdk_bdev_io_type_supported(ns->bdev, SPDK_BDEV_IO_TYPE_ZONE_APPEND)) {
			/* [한국어] zoned bdev이고 zone append IO 타입이 지원되는 NS 발견 */
			return true;
		}
	}

	return false;	/* [한국어] zone append 지원 NS 없음 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_add_ns_ext - subsystem에 NS(Namespace) 추가
 *
 * @subsystem:  대상 subsystem (INACTIVE 또는 PAUSED 상태)
 * @bdev_name:  backing storage가 될 bdev의 이름
 * @user_opts:  NS 옵션 (nsid, nguid, eui64, uuid, anagrpid, hide_metadata 등)
 * @opts_size:  user_opts 구조체 크기 (전후 호환을 위해 명시; 0=NULL)
 * @ptpl_file:  Persist Through Power Loss용 PR 영속화 파일 경로 (NULL=비영속)
 * @return: 성공 시 할당된 NSID(>0), 실패 시 0
 *
 * NS는 호스트가 NVMe 명령으로 read/write할 논리 단위이며, SPDK는 각 NS를
 * 하나의 bdev에 매핑한다. 이 함수는 다음을 수행한다:
 *   1) 빈 NSID slot 검색 (opts.nsid==0이면 자동 할당)
 *   2) bdev_open으로 backing bdev 핸들 획득 (write 가능)
 *   3) bdev metadata, zone, csi 등 호환성 검증
 *   4) ANA group 카운터 증가, transport에 NS 등록 통지
 *   5) ptpl_file이 있으면 PR 상태 복원
 *   6) NS를 모든 ctrlr에게 visible 설정 (no_auto_visible=false 가정)
 *
 * 사전 조건: subsystem이 INACTIVE 또는 PAUSED — ACTIVE 상태에선 거부.
 *
 * 호출 체인:
 *   RPC subsystem_add_ns → [spdk_nvmf_subsystem_add_ns_ext] → spdk_bdev_open_ext_v2
 *     → transport->ns_add (각 transport별) → nvmf_ctrlr_ns_set_visible
 */
uint32_t
spdk_nvmf_subsystem_add_ns_ext(struct spdk_nvmf_subsystem *subsystem, const char *bdev_name,
			       const struct spdk_nvmf_ns_opts *user_opts, size_t opts_size,
			       const char *ptpl_file)
{
	struct spdk_nvmf_transport *transport;		/* [한국어] NS 등록 통지 대상 transport 순회용 */
	struct spdk_nvmf_ns_opts opts;			/* [한국어] 내부 NS 옵션 (기본값으로 초기화 후 user_opts 적용) */
	struct spdk_bdev_open_opts open_opts = {};	/* [한국어] bdev open 옵션 (0 초기화) */
	struct spdk_nvmf_ns *ns, *first_ns;		/* [한국어] 새 NS / 첫 번째 기존 NS (FDP 호환성 검사용) */
	struct spdk_nvmf_ctrlr *ctrlr;			/* [한국어] ctrlr 순회용 (NS visible 설정) */
	struct spdk_nvmf_reservation_info info = {0};	/* [한국어] PTPL 파일에서 로드할 PR 상태 정보 */
	int rc;					/* [한국어] 에러 코드 */
	bool zone_append_supported;			/* [한국어] 이 NS bdev의 zone append 지원 여부 */
	uint64_t max_zone_append_size_kib;		/* [한국어] zone append 최대 크기 (KiB) */

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return 0;	/* [한국어] ACTIVE 상태에서는 NS 추가 불가 — 0 반환 (실패) */
	}

	spdk_nvmf_ns_opts_get_defaults(&opts, sizeof(opts));
	/* [한국어] opts를 기본값으로 초기화 (nsid=0, anagrpid=0 등) */
	if (user_opts) {
		nvmf_ns_opts_copy(&opts, user_opts, opts_size);
		/* [한국어] user_opts 내용을 ABI-안전하게 opts에 복사 */
	}

	if (opts.nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		SPDK_ERRLOG("Invalid NSID %" PRIu32 "\n", opts.nsid);
		return 0;	/* [한국어] 0xFFFFFFFF(Global NS Tag)는 NSID로 사용 불가 */
	}

	if (opts.nsid == 0) {
		/*
		 * NSID not specified - find a free index.
		 *
		 * If no free slots are found, return error.
		 */
		/* [한국어] NSID 자동 할당: 1부터 max_nsid까지 빈 슬롯 탐색 */
		for (opts.nsid = 1; opts.nsid <= subsystem->max_nsid; opts.nsid++) {
			if (_nvmf_subsystem_get_ns(subsystem, opts.nsid) == NULL) {
				break;	/* [한국어] 빈 슬롯 발견 */
			}
		}
		if (opts.nsid > subsystem->max_nsid) {
			SPDK_ERRLOG("No free namespace slot available in the subsystem\n");
			return 0;	/* [한국어] 모든 슬롯 사용 중 */
		}
	}

	if (opts.nsid > subsystem->max_nsid) {
		SPDK_ERRLOG("NSID greater than maximum not allowed\n");
		return 0;	/* [한국어] 요청 NSID가 max_nsid 초과 */
	}

	if (_nvmf_subsystem_get_ns(subsystem, opts.nsid)) {
		SPDK_ERRLOG("Requested NSID %" PRIu32 " already in use\n", opts.nsid);
		return 0;	/* [한국어] 이미 사용 중인 NSID */
	}

	if (opts.anagrpid == 0) {
		opts.anagrpid = opts.nsid;
		/* [한국어] ANA group ID 미지정 → NSID와 동일 (1:1 매핑) */
	}

	if (opts.anagrpid > subsystem->max_nsid) {
		SPDK_ERRLOG("ANAGRPID greater than maximum NSID not allowed\n");
		return 0;	/* [한국어] ANA group ID는 max_nsid 이하여야 함 */
	}

	ns = calloc(1, sizeof(*ns));		/* [한국어] 0 초기화 NS 구조체 할당 */
	if (ns == NULL) {
		SPDK_ERRLOG("Namespace allocation failed\n");
		return 0;
	}

	TAILQ_INIT(&ns->hosts);			/* [한국어] NS별 호스트 ACL TAILQ 초기화 */
	ns->always_visible = !opts.no_auto_visible;
	/* [한국어] no_auto_visible=false이면 NS는 모든 ctrlr에 자동 노출 */
	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		nvmf_ctrlr_ns_set_visible(ctrlr, opts.nsid, ns->always_visible);
		/* [한국어] 이미 connect된 ctrlr에게 새 NS visible 상태 설정 */
	}

	spdk_bdev_open_opts_init(&open_opts, sizeof(open_opts));
	/* [한국어] bdev open 옵션 기본값 초기화 */
	open_opts.hide_metadata = opts.hide_metadata;
	/* [한국어] hide_metadata=true이면 DIF/DIX metadata를 블록 데이터에서 숨김 */

	rc = spdk_bdev_open_ext_v2(bdev_name, true, nvmf_ns_event, ns, &open_opts, &ns->desc);
	/* [한국어] bdev를 write 가능 모드로 open (true=write), 이벤트 핸들러=nvmf_ns_event */
	if (rc != 0) {
		SPDK_ERRLOG("Subsystem %s: bdev %s cannot be opened, error=%d\n",
			    subsystem->subnqn, bdev_name, rc);
		free(ns);
		return 0;	/* [한국어] bdev open 실패 — 다른 모듈이 claim 중이거나 bdev 미존재 */
	}

	ns->bdev = spdk_bdev_desc_get_bdev(ns->desc);
	/* [한국어] open된 bdev descriptor에서 bdev 포인터 캐시 */

	if (spdk_bdev_desc_get_md_size(ns->desc) != 0) {
		/* [한국어] bdev에 metadata(DIF/DIX)가 있는 경우 — 타입 검사 */
		if (!spdk_bdev_desc_is_md_interleaved(ns->desc)) {
			SPDK_ERRLOG("Can't attach bdev with separate metadata.\n");
			spdk_bdev_close(ns->desc);
			free(ns);
			return 0;	/* [한국어] 분리 metadata(separate MD)는 미지원 — interleaved만 가능 */
		}

		if (spdk_bdev_desc_get_md_size(ns->desc) > SPDK_BDEV_MAX_INTERLEAVED_MD_SIZE) {
			SPDK_ERRLOG("Maximum supported interleaved md size %u, current md size %u\n",
				    SPDK_BDEV_MAX_INTERLEAVED_MD_SIZE,
				    spdk_bdev_desc_get_md_size(ns->desc));
			spdk_bdev_close(ns->desc);
			free(ns);
			return 0;	/* [한국어] interleaved metadata 크기 상한 초과 */
		}
	}

	rc = spdk_bdev_module_claim_bdev(ns->bdev, ns->desc, &ns_bdev_module);
	/* [한국어] ns_bdev_module 명의로 bdev를 독점 claim (다른 nvmf NS가 같은 bdev 사용 방지) */
	if (rc != 0) {
		spdk_bdev_close(ns->desc);
		free(ns);
		return 0;	/* [한국어] claim 실패 — 이미 다른 모듈이 claim 중 */
	}

	ns->passthru_nsid = spdk_bdev_get_nvme_nsid(ns->bdev);
	/* [한국어] passthrough 모드용: bdev_nvme의 원본 NVMe NSID를 캐시 */
	if (subsystem->passthrough && ns->passthru_nsid == 0) {
		SPDK_ERRLOG("Only bdev_nvme namespaces can be added to a passthrough subsystem.\n");
		goto err;	/* [한국어] passthrough subsystem에는 bdev_nvme NS만 추가 가능 */
	}

	/* Cache the zcopy capability of the bdev device */
	ns->zcopy = spdk_bdev_io_type_supported(ns->bdev, SPDK_BDEV_IO_TYPE_ZCOPY);
	/* [한국어] Zero-Copy I/O 지원 여부 캐시 — IO path에서 zcopy 최적화 여부 결정 */

	if (spdk_uuid_is_null(&opts.uuid)) {
		opts.uuid = *spdk_bdev_get_uuid(ns->bdev);
		/* [한국어] UUID 미지정 → bdev의 UUID를 NS UUID로 사용 */
	}

	/* if nguid descriptor is supported by bdev module (nvme) then uuid = nguid */
	if (spdk_mem_all_zero(opts.nguid, sizeof(opts.nguid))) {
		SPDK_STATIC_ASSERT(sizeof(opts.nguid) == sizeof(opts.uuid), "size mismatch");
		memcpy(opts.nguid, spdk_bdev_get_uuid(ns->bdev), sizeof(opts.nguid));
		/* [한국어] NGUID 미지정 → bdev UUID를 NGUID로 복사 (NVMe spec: NGUID=UUID로 사용 가능) */
	}

	if (spdk_bdev_is_zoned(ns->bdev)) {
		/* [한국어] ZNS(Zoned Namespace Storage) bdev인 경우 */
		SPDK_DEBUGLOG(nvmf, "The added namespace is backed by a zoned block device.\n");
		ns->csi = SPDK_NVME_CSI_ZNS;		/* [한국어] Command Set Identifier를 ZNS로 설정 */

		zone_append_supported = spdk_bdev_io_type_supported(ns->bdev,
					SPDK_BDEV_IO_TYPE_ZONE_APPEND);
		/* [한국어] 이 NS의 zone append 지원 여부 */
		max_zone_append_size_kib = spdk_bdev_get_max_zone_append_size(ns->bdev) *
					   spdk_bdev_desc_get_block_size(ns->desc);
		/* [한국어] zone append 최대 크기 = block count × block size (bytes) → KiB 단위 */

		if (_nvmf_subsystem_get_first_zoned_ns(subsystem) != NULL &&
		    (nvmf_subsystem_zone_append_supported(subsystem) != zone_append_supported ||
		     subsystem->max_zone_append_size_kib != max_zone_append_size_kib)) {
			SPDK_ERRLOG("Namespaces with different zone append support or different zone append size are not allowed.\n");
			goto err;
			/* [한국어] 기존 zoned NS와 zone append 지원/크기가 다르면 추가 불가 (규격 일관성) */
		}

		subsystem->max_zone_append_size_kib = max_zone_append_size_kib;
		/* [한국어] subsystem의 zone append 크기 캐시 업데이트 */
	}

	first_ns = spdk_nvmf_subsystem_get_first_ns(subsystem);
	/* [한국어] FDP 호환성 검사를 위해 첫 번째 기존 NS 참조 */
	if (!first_ns) {
		/* [한국어] 첫 번째 NS 추가 시 — FDP 지원 여부 결정 */
		if (spdk_bdev_get_nvme_ctratt(ns->bdev).bits.fdps) {
			SPDK_DEBUGLOG(nvmf, "Subsystem with id: %u has FDP capability.\n",
				      subsystem->id);
			subsystem->fdp_supported = true;
			/* [한국어] FDP(Flexible Data Placement) 지원 bdev → subsystem FDP 활성화 */
		}
	} else {
		/* [한국어] 기존 NS가 있는 경우 — 신규 NS의 FDP 지원이 일치해야 함 */
		if (spdk_bdev_get_nvme_ctratt(first_ns->bdev).bits.fdps !=
		    spdk_bdev_get_nvme_ctratt(ns->bdev).bits.fdps) {
			SPDK_ERRLOG("Subsystem with id: %u can%s FDP namespace.\n", subsystem->id,
				    spdk_bdev_get_nvme_ctratt(first_ns->bdev).bits.fdps ? " only add" : "not add");
			goto err;	/* [한국어] FDP 지원 여부 불일치 — 추가 불가 */
		}
	}

	ns->opts = opts;			/* [한국어] NS opts 저장 */
	ns->subsystem = subsystem;		/* [한국어] 역참조 — 이벤트 핸들러에서 subsystem 접근 */
	subsystem->ns[opts.nsid - 1] = ns;	/* [한국어] subsystem NS 배열에 등록 (1-based NSID → 0-based 인덱스) */
	ns->nsid = opts.nsid;			/* [한국어] NSID 저장 */
	ns->anagrpid = opts.anagrpid;		/* [한국어] ANA group ID 저장 */
	subsystem->ana_group[ns->anagrpid - 1]++;
	/* [한국어] ANA group 참조 카운터 증가 (group당 NS 수 추적) */
	TAILQ_INIT(&ns->registrants);		/* [한국어] PR 등록자 목록 초기화 */
	STAILQ_INIT(&ns->reservations);	/* [한국어] 대기 중인 reservation 커맨드 큐 초기화 */
	if (ptpl_file) {
		ns->ptpl_file = strdup(ptpl_file);
		/* [한국어] PTPL(Persist Through Power Loss) 파일 경로 복사 — PR 영속화 */
		if (!ns->ptpl_file) {
			SPDK_ERRLOG("Namespace ns->ptpl_file allocation failed\n");
			goto err;
		}
	}

	if (nvmf_ns_is_ptpl_capable(ns)) {
		/* [한국어] ptpl_file이 설정되어 있으면 기존 PR 상태 로드 및 복원 */
		rc = nvmf_ns_reservation_load(ns, &info);
		/* [한국어] ptpl_file에서 JSON PR 상태를 info에 파싱 */
		if (rc) {
			SPDK_ERRLOG("Subsystem load reservation failed\n");
			goto err;
		}

		rc = nvmf_ns_reservation_restore(ns, &info);
		/* [한국어] 파싱된 info를 ns에 적용 (registrant 목록, holder, rtype 복원) */
		if (rc) {
			SPDK_ERRLOG("Subsystem restore reservation failed\n");
			goto err;
		}
	}

	for (transport = spdk_nvmf_transport_get_first(subsystem->tgt); transport;
	     transport = spdk_nvmf_transport_get_next(transport)) {
		/* [한국어] 모든 transport에 NS 추가 통지 */
		if (transport->ops->subsystem_add_ns) {
			rc = transport->ops->subsystem_add_ns(transport, subsystem, ns);
			/* [한국어] transport별 NS 등록 (예: TCP에서 NS별 ioat 채널 설정) */
			if (rc) {
				SPDK_ERRLOG("Namespace attachment is not allowed by %s transport\n", transport->ops->name);
				nvmf_ns_reservation_clear_all_registrants(ns);
				goto err;	/* [한국어] transport 거부 — PR 정리 후 에러 반환 */
			}
		}
	}

	/* JSON value obj is freed before sending the response. Set NULL to prevent usage of dangling pointer. */
	ns->opts.transport_specific = NULL;
	/* [한국어] transport_specific은 RPC JSON 파싱 중의 임시 포인터 — NULL로 dangling 방지 */

	SPDK_DEBUGLOG(nvmf, "Subsystem %s: bdev %s assigned nsid %" PRIu32 "\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem),
		      bdev_name,
		      opts.nsid);

	nvmf_subsystem_ns_changed(subsystem, opts.nsid);
	/* [한국어] NS 추가 성공 — 모든 ctrlr에게 NS 변경 AER 발송 */

	SPDK_DTRACE_PROBE2(nvmf_subsystem_add_ns, subsystem->subnqn, ns->nsid);
	/* [한국어] DTrace 프로브: NS 추가 이벤트 기록 */

	return opts.nsid;		/* [한국어] 성공 — 할당된 NSID 반환 */
err:
	subsystem->ns[opts.nsid - 1] = NULL;		/* [한국어] NS 배열 등록 취소 */
	spdk_bdev_module_release_bdev(ns->bdev);	/* [한국어] bdev claim 해제 */
	spdk_bdev_close(ns->desc);			/* [한국어] bdev descriptor close */
	free(ns->ptpl_file);				/* [한국어] ptpl 파일 경로 문자열 해제 */
	free(ns);					/* [한국어] NS 구조체 해제 */

	return 0;	/* [한국어] 실패 — 0 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_set_ns_ana_group - NS의 ANA group ID 변경
 *
 * @subsystem: 대상 subsystem
 * @nsid:      변경할 NS의 NSID (1-based)
 * @anagrpid:  새 ANA group ID (1~max_nsid, 0 불가)
 * @return: 0=성공, -1=유효하지 않은 파라미터
 *
 * ANA group ID는 NS가 어느 ANA group에 속하는지를 나타낸다.
 * 각 listener의 ana_state 배열은 [anagrpid-1] 인덱스로 해당 NS의 ANA 상태를 반환.
 * 변경 시 기존 group 카운터를 감소, 새 group 카운터를 증가시킨다.
 */
int
spdk_nvmf_subsystem_set_ns_ana_group(struct spdk_nvmf_subsystem *subsystem,
				     uint32_t nsid, uint32_t anagrpid)
{
	struct spdk_nvmf_ns *ns;	/* [한국어] 변경 대상 NS */

	if (anagrpid > subsystem->max_nsid) {
		SPDK_ERRLOG("ANAGRPID greater than maximum NSID not allowed\n");
		return -1;	/* [한국어] ANA group ID 범위 초과 */
	}

	if (anagrpid == 0) {
		SPDK_ERRLOG("Zero is not allowed to ANAGRPID\n");
		return -1;	/* [한국어] 0은 "미지정"을 의미하므로 명시적 설정 불허 */
	}

	if (nsid == 0 || nsid > subsystem->max_nsid) {
		return -1;	/* [한국어] 유효하지 않은 NSID */
	}

	ns = subsystem->ns[nsid - 1];	/* [한국어] NS 배열에서 포인터 획득 */
	if (!ns) {
		return -1;	/* [한국어] 해당 NSID에 NS 없음 */
	}

	assert(ns->anagrpid - 1 < subsystem->max_nsid);
	/* [한국어] 현재 ANA group ID가 유효 범위 내 */

	assert(subsystem->ana_group[ns->anagrpid - 1] > 0);
	/* [한국어] 현재 group의 참조 카운터가 0보다 커야 함 */

	subsystem->ana_group[ns->anagrpid - 1]--;
	/* [한국어] 기존 ANA group 참조 카운터 감소 */

	subsystem->ana_group[anagrpid - 1]++;
	/* [한국어] 새 ANA group 참조 카운터 증가 */

	ns->anagrpid = anagrpid;		/* [한국어] NS의 ANA group ID 갱신 */
	ns->opts.anagrpid = anagrpid;		/* [한국어] opts에도 동기화 (직렬화 시 사용) */

	nvmf_subsystem_ns_changed(subsystem, nsid);
	/* [한국어] 모든 ctrlr에게 NS 변경 AER 발송 (Identify NS Active List 재조회 유도) */

	return 0;
}

/*
 * [한국어]
 * nvmf_subsystem_get_next_allocated_nsid - 이전 NSID 이후의 다음 할당된 NSID 반환
 *
 * @subsystem: 탐색할 subsystem
 * @prev_nsid: 시작점 NSID (이 NSID 이후부터 탐색, 0이면 처음부터)
 * @return: 다음 할당된 NSID (>0), 없으면 0
 *
 * subsystem의 NS 배열을 선형 탐색하여 할당된(NULL이 아닌) NSID를 찾는다.
 * spdk_nvmf_subsystem_get_first/next_ns의 내부 구현에 사용.
 */
static uint32_t
nvmf_subsystem_get_next_allocated_nsid(struct spdk_nvmf_subsystem *subsystem,
				       uint32_t prev_nsid)
{
	uint32_t nsid;	/* [한국어] 탐색 중인 NSID */

	if (prev_nsid >= subsystem->max_nsid) {
		return 0;	/* [한국어] 이미 마지막 NSID에 도달 — 다음 없음 */
	}

	for (nsid = prev_nsid + 1; nsid <= subsystem->max_nsid; nsid++) {
		if (subsystem->ns[nsid - 1]) {
			return nsid;	/* [한국어] 할당된 NS 발견 */
		}
	}

	return 0;	/* [한국어] 더 이상 할당된 NS 없음 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_first_ns - subsystem의 첫 번째 NS 반환
 *
 * @subsystem: 조회할 subsystem
 * @return: 첫 번째 할당된 NS, 없으면 NULL
 *
 * NS 목록 순회의 시작점. RPC/management API에서 NS 열거 시 사용.
 */
struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_first_ns(struct spdk_nvmf_subsystem *subsystem)
{
	uint32_t first_nsid;	/* [한국어] 첫 번째 할당된 NSID */

	first_nsid = nvmf_subsystem_get_next_allocated_nsid(subsystem, 0);
	/* [한국어] NSID 0 이후부터 탐색 (즉 NSID 1부터) */
	return _nvmf_subsystem_get_ns(subsystem, first_nsid);
	/* [한국어] first_nsid=0이면 NULL 반환 (NS 없음) */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_next_ns - 다음 NS 반환
 *
 * @prev_ns: 현재 NS
 * @return: 다음 할당된 NS, 없으면 NULL
 */
struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem,
				struct spdk_nvmf_ns *prev_ns)
{
	uint32_t next_nsid;	/* [한국어] 다음 할당된 NSID */

	next_nsid = nvmf_subsystem_get_next_allocated_nsid(subsystem, prev_ns->opts.nsid);
	/* [한국어] prev_ns의 NSID 이후부터 탐색 */
	return _nvmf_subsystem_get_ns(subsystem, next_nsid);
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_ns - NSID로 NS 직접 조회
 *
 * @nsid: 조회할 NSID (1-based)
 * @return: 해당 NSID의 NS, 없거나 유효하지 않으면 NULL
 */
struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	return _nvmf_subsystem_get_ns(subsystem, nsid);
	/* [한국어] 내부 _nvmf_subsystem_get_ns에 위임 (범위 검사 포함) */
}

/*
 * [한국어]
 * spdk_nvmf_ns_get_id - NS의 NSID 반환
 *
 * @ns: 조회할 NS
 * @return: 1-based NSID
 */
uint32_t
spdk_nvmf_ns_get_id(const struct spdk_nvmf_ns *ns)
{
	return ns->opts.nsid;	/* [한국어] opts에 저장된 NSID 직접 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_ns_get_bdev - NS의 backing bdev 반환
 *
 * @ns: 조회할 NS
 * @return: bdev 포인터 (NS 수명 동안 유효)
 */
struct spdk_bdev *
spdk_nvmf_ns_get_bdev(struct spdk_nvmf_ns *ns)
{
	return ns->bdev;	/* [한국어] bdev 포인터 직접 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_ns_get_opts - NS 옵션 복사 반환
 *
 * @ns:        조회할 NS
 * @opts:      복사 대상 옵션 구조체 (호출자가 할당)
 * @opts_size: 호출자 opts 크기 (ABI versioning)
 *
 * ns->opts를 opts_size 범위만큼 복사한다. 구조체 크기 불일치 시 안전.
 */
void
spdk_nvmf_ns_get_opts(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_ns_opts *opts,
		      size_t opts_size)
{
	memset(opts, 0, opts_size);				/* [한국어] 대상 초기화 */
	memcpy(opts, &ns->opts, spdk_min(sizeof(ns->opts), opts_size));
	/* [한국어] 두 크기 중 작은 쪽만큼 복사 (버전 불일치 시 버퍼 오버런 방지) */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_sn - subsystem의 Serial Number(SN) 반환
 *
 * @return: SN 문자열 (최대 20자, NULL 종결)
 */
const char *
spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->sn;	/* [한국어] 고정 크기 배열 직접 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_set_sn - subsystem의 Serial Number 설정
 *
 * @sn: 새 SN 문자열 (최대 20자, ASCII만 허용)
 * @return: 0=성공, -1=길이 초과 또는 ASCII 검증 실패
 *
 * NVMe spec: SN은 NVMe Controller Identify의 바이트 4-23에 보고되는
 * 20자 ASCII 문자열. 비-ASCII 또는 길이 초과 시 거부.
 */
int
spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn)
{
	size_t len, max_len;	/* [한국어] 입력 길이 / 최대 허용 길이 */

	max_len = sizeof(subsystem->sn) - 1;	/* [한국어] NULL 종결 포함 고려 */
	len = strlen(sn);
	if (len > max_len) {
		SPDK_DEBUGLOG(nvmf, "Invalid sn \"%s\": length %zu > max %zu\n",
			      sn, len, max_len);
		return -1;	/* [한국어] NVMe spec 20자 제한 초과 */
	}

	if (!nvmf_valid_ascii_string(sn, len)) {
		SPDK_DEBUGLOG(nvmf, "Non-ASCII sn\n");
		SPDK_LOGDUMP(nvmf, "sn", sn, len);
		return -1;	/* [한국어] ASCII 범위 벗어난 문자 포함 */
	}

	snprintf(subsystem->sn, sizeof(subsystem->sn), "%s", sn);
	/* [한국어] SN 복사 (snprintf로 overflow 방지) */

	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_mn - subsystem의 Model Number(MN) 반환
 *
 * @return: MN 문자열 (최대 40자, NULL 종결)
 */
const char *
spdk_nvmf_subsystem_get_mn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->mn;	/* [한국어] 고정 크기 배열 직접 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_set_mn - subsystem의 Model Number 설정
 *
 * @mn: 새 MN 문자열 (최대 40자, ASCII만 허용; NULL이면 기본값 사용)
 * @return: 0=성공, -1=길이 초과 또는 ASCII 검증 실패
 *
 * NVMe spec: MN은 NVMe Controller Identify의 바이트 24-63에 보고되는
 * 40자 ASCII 문자열.
 */
int
spdk_nvmf_subsystem_set_mn(struct spdk_nvmf_subsystem *subsystem, const char *mn)
{
	size_t len, max_len;	/* [한국어] 입력 길이 / 최대 허용 길이 */

	if (mn == NULL) {
		mn = MODEL_NUMBER_DEFAULT;	/* [한국어] NULL이면 기본 MN 문자열 사용 */
	}
	max_len = sizeof(subsystem->mn) - 1;	/* [한국어] NULL 종결 포함 고려 */
	len = strlen(mn);
	if (len > max_len) {
		SPDK_DEBUGLOG(nvmf, "Invalid mn \"%s\": length %zu > max %zu\n",
			      mn, len, max_len);
		return -1;	/* [한국어] NVMe spec 40자 제한 초과 */
	}

	if (!nvmf_valid_ascii_string(mn, len)) {
		SPDK_DEBUGLOG(nvmf, "Non-ASCII mn\n");
		SPDK_LOGDUMP(nvmf, "mn", mn, len);
		return -1;	/* [한국어] ASCII 범위 벗어난 문자 포함 */
	}

	snprintf(subsystem->mn, sizeof(subsystem->mn), "%s", mn);
	/* [한국어] MN 복사 (snprintf로 overflow 방지) */

	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_nqn - subsystem의 NQN(NVMe Qualified Name) 반환
 *
 * @return: NQN 문자열 (NULL 종결, subsystem 수명 동안 유효)
 */
const char *
spdk_nvmf_subsystem_get_nqn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subnqn;	/* [한국어] 고정 크기 배열 직접 반환 */
}

/* We have to use the typedef in the function declaration to appease astyle. */
/* [한국어] astyle 포맷터 호환을 위해 enum 직접 사용 대신 typedef 사용 */
typedef enum spdk_nvmf_subtype spdk_nvmf_subtype_t;

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_type - subsystem 타입 반환
 *
 * @return: SPDK_NVMF_SUBTYPE_NVME(I/O subsystem) 또는 SPDK_NVMF_SUBTYPE_DISCOVERY
 */
spdk_nvmf_subtype_t
spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subtype;	/* [한국어] 서브타입 직접 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_max_nsid - subsystem의 최대 NSID 반환
 *
 * @return: max_nsid (NS 배열 크기와 같음)
 */
uint32_t
spdk_nvmf_subsystem_get_max_nsid(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->max_nsid;	/* [한국어] 직접 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_set_cntlid_range - ctrlr ID(CNTLID) 할당 범위 설정
 *
 * @subsystem:  대상 subsystem (INACTIVE 상태여야 함)
 * @min_cntlid: 최소 CNTLID (NVMF_MIN_CNTLID 이상)
 * @max_cntlid: 최대 CNTLID (NVMF_MAX_CNTLID 이하, min_cntlid 이상)
 * @return: 0=성공, -EAGAIN(INACTIVE 아님), -EINVAL(범위 오류)
 *
 * NVMe spec: CNTLID 범위 FFF0h~FFFFh는 예약됨 (NVMF_MIN_CNTLID..NVMF_MAX_CNTLID로 제한).
 * 범위 변경 후 next_cntlid가 범위 밖이면 min_cntlid로 리셋.
 */
int
spdk_nvmf_subsystem_set_cntlid_range(struct spdk_nvmf_subsystem *subsystem,
				     uint16_t min_cntlid, uint16_t max_cntlid)
{
	if (subsystem->state != SPDK_NVMF_SUBSYSTEM_INACTIVE) {
		return -EAGAIN;		/* [한국어] INACTIVE 상태에서만 변경 허용 */
	}

	if (min_cntlid > max_cntlid) {
		return -EINVAL;		/* [한국어] min이 max보다 크면 유효하지 않은 범위 */
	}
	/* The spec reserves cntlid values in the range FFF0h to FFFFh. */
	if (min_cntlid < NVMF_MIN_CNTLID || min_cntlid > NVMF_MAX_CNTLID ||
	    max_cntlid < NVMF_MIN_CNTLID || max_cntlid > NVMF_MAX_CNTLID) {
		return -EINVAL;		/* [한국어] 스펙 예약 범위(FFF0h~FFFFh) 침범 방지 */
	}
	subsystem->min_cntlid = min_cntlid;	/* [한국어] 최소 CNTLID 업데이트 */
	subsystem->max_cntlid = max_cntlid;	/* [한국어] 최대 CNTLID 업데이트 */
	if (subsystem->next_cntlid < min_cntlid || subsystem->next_cntlid > max_cntlid) {
		subsystem->next_cntlid = min_cntlid;
		/* [한국어] next_cntlid가 새 범위 밖이면 min_cntlid로 리셋 */
	}

	return 0;
}

/*
 * [한국어]
 * nvmf_subsystem_gen_cntlid - 새 ctrlr에 할당할 미사용 CNTLID 생성
 *
 * @subsystem: CNTLID를 할당할 subsystem
 * @return: 미사용 CNTLID(1~FFEFh), 모두 사용 중이면 0xFFFF(오류 코드)
 *
 * next_cntlid에서 시작하여 환형 탐색으로 미사용 CNTLID를 찾는다.
 * 최대 (max_cntlid - min_cntlid + 1) 횟수 탐색 후 미발견이면 0xFFFF 반환.
 * 실행 컨텍스트: nvmf_ctrlr connect 처리 중 (단일 스레드, 락 불필요).
 */
uint16_t
nvmf_subsystem_gen_cntlid(struct spdk_nvmf_subsystem *subsystem)
{
	int count;		/* [한국어] 탐색 횟수 카운터 */
	uint16_t cntlid;	/* [한국어] 현재 시도 중인 CNTLID */

	/*
	 * In the worst case, we might have to try all CNTLID values between min_cntlid and max_cntlid
	 * before we find one that is unused (or find that all values are in use).
	 */
	for (count = 0; count < subsystem->max_cntlid - subsystem->min_cntlid + 1; count++) {
		/* [한국어] 최대 범위 크기만큼 반복 (환형 탐색) */
		cntlid = subsystem->next_cntlid;	/* [한국어] 현재 후보 CNTLID */
		subsystem->next_cntlid++;		/* [한국어] 다음 후보로 전진 */

		if (subsystem->next_cntlid > subsystem->max_cntlid) {
			subsystem->next_cntlid = subsystem->min_cntlid;
			/* [한국어] 상한 초과 시 min_cntlid로 랩어라운드 (환형 탐색) */
		}

		/* Check if a controller with this cntlid currently exists. */
		if (nvmf_subsystem_get_ctrlr(subsystem, cntlid) == NULL) {
			/* Found unused cntlid */
			return cntlid;		/* [한국어] 미사용 CNTLID 발견 — 반환 */
		}
	}

	/* All valid cntlid values are in use. */
	return 0xFFFF;		/* [한국어] 사용 가능한 CNTLID 없음 — 오류 코드 */
}

/*
 * [한국어]
 * nvmf_subsystem_add_ctrlr - subsystem에 ctrlr 등록
 *
 * @subsystem: 대상 subsystem
 * @ctrlr:     등록할 ctrlr
 * @return: 0=성공, -EBUSY(CNTLID 할당 실패), -EEXIST(정적 CNTLID 충돌)
 *
 * 동적 ctrlr(dynamic_ctrlr=true)이면 CNTLID를 자동 생성하고,
 * 정적 ctrlr이면 지정된 CNTLID가 이미 사용 중인지 확인 후 등록.
 * 실행 컨텍스트: subsystem->thread (connect 처리 흐름).
 */
int
nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr)
{

	if (ctrlr->dynamic_ctrlr) {
		/* [한국어] 동적 ctrlr: 자동 CNTLID 할당 */
		ctrlr->cntlid = nvmf_subsystem_gen_cntlid(subsystem);
		if (ctrlr->cntlid == 0xFFFF) {
			/* Unable to get a cntlid */
			SPDK_ERRLOG("Reached max simultaneous ctrlrs\n");
			return -EBUSY;		/* [한국어] 모든 CNTLID 사용 중 */
		}
	} else if (nvmf_subsystem_get_ctrlr(subsystem, ctrlr->cntlid) != NULL) {
		SPDK_ERRLOG("Ctrlr with cntlid %u already exist\n", ctrlr->cntlid);
		return -EEXIST;		/* [한국어] 정적 CNTLID 충돌 — 이미 동일 CNTLID의 ctrlr 존재 */
	}

	TAILQ_INSERT_TAIL(&subsystem->ctrlrs, ctrlr, link);
	/* [한국어] ctrlr를 subsystem의 TAILQ 말미에 삽입 */

	SPDK_DTRACE_PROBE3(nvmf_subsystem_add_ctrlr, subsystem->subnqn, ctrlr, ctrlr->hostnqn);
	/* [한국어] DTrace 프로브: ctrlr 추가 이벤트 기록 */

	return 0;
}

/*
 * [한국어]
 * nvmf_subsystem_remove_ctrlr - subsystem에서 ctrlr 제거
 *
 * @subsystem: 대상 subsystem
 * @ctrlr:     제거할 ctrlr
 *
 * ctrlr disconnect 완료 시 TAILQ에서 제거.
 * 실행 컨텍스트: subsystem->thread (assert로 검증).
 */
void
nvmf_subsystem_remove_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			    struct spdk_nvmf_ctrlr *ctrlr)
{
	SPDK_DTRACE_PROBE3(nvmf_subsystem_remove_ctrlr, subsystem->subnqn, ctrlr, ctrlr->hostnqn);
	/* [한국어] DTrace 프로브: ctrlr 제거 이벤트 기록 */

	assert(spdk_get_thread() == subsystem->thread);	/* [한국어] subsystem thread에서 실행 검증 */
	assert(subsystem == ctrlr->subsys);		/* [한국어] ctrlr가 이 subsystem 소속 검증 */
	SPDK_DEBUGLOG(nvmf, "remove ctrlr %p id 0x%x from subsys %p %s\n", ctrlr, ctrlr->cntlid, subsystem,
		      subsystem->subnqn);
	TAILQ_REMOVE(&subsystem->ctrlrs, ctrlr, link);	/* [한국어] ctrlr TAILQ에서 제거 */
}

/*
 * [한국어]
 * nvmf_subsystem_get_ctrlr - CNTLID로 ctrlr 조회
 *
 * @subsystem: 대상 subsystem
 * @cntlid:    찾을 CNTLID
 * @return: 해당 CNTLID의 ctrlr, 없으면 NULL
 *
 * 선형 탐색 O(n). ctrlr 수가 많지 않아 배열 대신 TAILQ 탐색으로 충분.
 */
struct spdk_nvmf_ctrlr *
nvmf_subsystem_get_ctrlr(struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid)
{
	struct spdk_nvmf_ctrlr *ctrlr;	/* [한국어] 순회 중인 ctrlr */

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		if (ctrlr->cntlid == cntlid) {
			return ctrlr;	/* [한국어] CNTLID 일치 */
		}
	}

	return NULL;	/* [한국어] 미발견 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_max_namespaces - subsystem의 최대 NS 수 반환
 *
 * @return: max_nsid (NS 배열 크기 = 지원 가능한 최대 NSID)
 */
uint32_t
spdk_nvmf_subsystem_get_max_namespaces(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->max_nsid;	/* [한국어] 직접 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_min_cntlid - 최소 CNTLID 반환
 */
uint16_t
spdk_nvmf_subsystem_get_min_cntlid(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->min_cntlid;	/* [한국어] 직접 반환 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_max_cntlid - 최대 CNTLID 반환
 */
uint16_t
spdk_nvmf_subsystem_get_max_cntlid(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->max_cntlid;	/* [한국어] 직접 반환 */
}

/* [한국어] PTPL(Persist Through Power Loss) JSON 직렬화/역직렬화를 위한 내부 구조체들.
 * 이 구조체들은 PTPL 파일 포맷과 1:1로 대응하며, spdk_nvmf_reservation_info와는
 * 분리된 중간 표현이다. JSON 파싱 후 spdk_nvmf_reservation_info로 변환된다. */

struct _nvmf_ns_registrant {
	uint64_t		rkey;
	/* [한국어] 등록자의 Reservation Key(RKEY).
	 * 설정자: nvmf_decode_ns_pr_reg에서 JSON "rkey" 필드로 파싱.
	 * 읽는 자: nvmf_ns_reservation_restore에서 등록자 복원 시.
	 * 값 범위: 호스트가 REGISTER 커맨드에서 지정한 64비트 키.
	 * 동기화: PTPL 로드/복원은 NS 초기화 시 단일 스레드에서 수행. */

	char			*host_uuid;
	/* [한국어] 등록자 호스트의 UUID 문자열 (동적 할당, spdk_json_decode_string).
	 * 설정자: nvmf_decode_ns_pr_reg에서 JSON "host_uuid" 필드로 파싱.
	 * 읽는 자: nvmf_ns_reservation_restore에서 spdk_uuid_parse 후 등록자 복원.
	 * 값 범위: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" 형식 UUID 문자열 (NULL 가능).
	 * 동기화: 파싱 후 복원까지 불변 (PTPL 로드 컨텍스트 내). 복원 후 free. */
};

struct _nvmf_ns_registrants {
	size_t				num_regs;
	/* [한국어] 파싱된 등록자 수.
	 * 설정자: nvmf_decode_ns_pr_regs에서 spdk_json_decode_array가 설정.
	 * 읽는 자: nvmf_ns_reservation_load_json에서 상한 검사,
	 *          nvmf_ns_reservation_restore에서 복원 루프 범위.
	 * 값 범위: 0~SPDK_NVMF_MAX_NUM_REGISTRANTS.
	 * 동기화: PTPL 로드 컨텍스트 내 단일 스레드. */

	struct _nvmf_ns_registrant	reg[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	/* [한국어] 각 등록자의 RKEY + host_uuid 배열.
	 * 설정자: nvmf_decode_ns_pr_reg가 각 요소를 채움.
	 * 읽는 자: nvmf_ns_reservation_restore에서 등록자 복원.
	 * 값 범위: num_regs 개 유효 (나머지는 미초기화).
	 * 동기화: PTPL 로드 컨텍스트 내 단일 스레드. */
};

struct _nvmf_ns_reservation {
	bool					ptpl_activated;
	/* [한국어] PTPL 활성 여부 (JSON "ptpl" 필드).
	 * false이면 power loss 후 PR 상태를 복원하지 않음.
	 * 설정자: nvmf_ns_pr_decoders의 "ptpl" optional 필드.
	 * 읽는 자: nvmf_ns_reservation_restore에서 복원 진행 여부 결정.
	 * 동기화: PTPL 로드 컨텍스트 내 단일 스레드. */

	enum spdk_nvme_reservation_type		rtype;
	/* [한국어] Reservation Type (JSON "rtype" optional 필드).
	 * 값 범위: WRITE_EXCLUSIVE(1)/EXCLUSIVE_ACCESS(2)/... 등 NVMe spec enum.
	 * 설정자: nvmf_ns_pr_decoders의 "rtype" optional 필드.
	 * 읽는 자: nvmf_ns_reservation_restore에서 ns->rtype 복원.
	 * 동기화: PTPL 로드 컨텍스트 내 단일 스레드. */

	uint64_t				crkey;
	/* [한국어] Current Reservation Key (JSON "crkey" optional 필드).
	 * 설정자: nvmf_ns_pr_decoders의 "crkey" optional 필드.
	 * 읽는 자: nvmf_ns_reservation_restore에서 ns->crkey 복원, 유효성 검사.
	 * 동기화: PTPL 로드 컨텍스트 내 단일 스레드. */

	char					*bdev_uuid;
	/* [한국어] NS backing bdev의 UUID 문자열 (동적 할당).
	 * 설정자: nvmf_ns_pr_decoders의 "bdev_uuid" 필수 필드.
	 * 읽는 자: nvmf_ns_reservation_restore에서 현재 bdev UUID와 일치 여부 검증.
	 * 동기화: PTPL 로드 컨텍스트 내 단일 스레드. 로드 후 free. */

	char					*holder_uuid;
	/* [한국어] reservation holder 호스트의 UUID 문자열 (동적 할당, optional).
	 * 설정자: nvmf_ns_pr_decoders의 "holder_uuid" optional 필드.
	 * 읽는 자: nvmf_ns_reservation_restore에서 holder 등록자 탐색.
	 * 동기화: PTPL 로드 컨텍스트 내 단일 스레드. 로드 후 free. */

	struct _nvmf_ns_registrants		regs;
	/* [한국어] 모든 등록자 목록 (JSON "registrants" 배열).
	 * 설정자: nvmf_ns_pr_decoders의 "registrants" 필드, nvmf_decode_ns_pr_regs로 파싱.
	 * 읽는 자: nvmf_ns_reservation_restore에서 각 등록자 복원.
	 * 동기화: PTPL 로드 컨텍스트 내 단일 스레드. */
};

/* [한국어] JSON의 각 registrant 객체를 _nvmf_ns_registrant로 파싱하는 디코더 배열.
 * {"rkey": <uint64>, "host_uuid": <string>} 형식. */
static const struct spdk_json_object_decoder nvmf_ns_pr_reg_decoders[] = {
	{"rkey", offsetof(struct _nvmf_ns_registrant, rkey), spdk_json_decode_uint64},
	{"host_uuid", offsetof(struct _nvmf_ns_registrant, host_uuid), spdk_json_decode_string},
};

/*
 * [한국어]
 * nvmf_decode_ns_pr_reg - JSON 값 하나를 _nvmf_ns_registrant로 파싱
 *
 * @val: JSON 객체 값 ({"rkey":..., "host_uuid":...})
 * @out: 파싱 결과를 채울 _nvmf_ns_registrant 포인터
 * @return: 0=성공, 음수=파싱 오류
 *
 * spdk_json_decode_array의 element_decode_func로 사용.
 */
static int
nvmf_decode_ns_pr_reg(const struct spdk_json_val *val, void *out)
{
	struct _nvmf_ns_registrant *reg = out;	/* [한국어] 파싱 결과 대상 */

	return spdk_json_decode_object(val, nvmf_ns_pr_reg_decoders,
				       SPDK_COUNTOF(nvmf_ns_pr_reg_decoders), reg);
	/* [한국어] nvmf_ns_pr_reg_decoders 규칙으로 JSON 객체 파싱 */
}

/*
 * [한국어]
 * nvmf_decode_ns_pr_regs - JSON 배열을 _nvmf_ns_registrants로 파싱
 *
 * @val: JSON 배열 값 ([{...}, {...}, ...])
 * @out: 파싱 결과를 채울 _nvmf_ns_registrants 포인터
 * @return: 0=성공, 음수=파싱 오류
 *
 * nvmf_ns_pr_decoders의 "registrants" 필드 디코더로 사용.
 */
static int
nvmf_decode_ns_pr_regs(const struct spdk_json_val *val, void *out)
{
	struct _nvmf_ns_registrants *regs = out;	/* [한국어] 파싱 결과 대상 */

	return spdk_json_decode_array(val, nvmf_decode_ns_pr_reg, regs->reg,
				      SPDK_NVMF_MAX_NUM_REGISTRANTS, &regs->num_regs,
				      sizeof(struct _nvmf_ns_registrant));
	/* [한국어] 배열 최대 SPDK_NVMF_MAX_NUM_REGISTRANTS개, 파싱된 수는 num_regs에 저장 */
}

/* [한국어] JSON의 전체 reservation 객체를 _nvmf_ns_reservation으로 파싱하는 디코더 배열.
 * optional(true) 필드는 누락되어도 파싱 에러 아님. */
static const struct spdk_json_object_decoder nvmf_ns_pr_decoders[] = {
	{"ptpl", offsetof(struct _nvmf_ns_reservation, ptpl_activated), spdk_json_decode_bool, true},
	/* [한국어] PTPL 활성 여부 — optional */
	{"rtype", offsetof(struct _nvmf_ns_reservation, rtype), spdk_json_decode_uint32, true},
	/* [한국어] Reservation Type — optional */
	{"crkey", offsetof(struct _nvmf_ns_reservation, crkey), spdk_json_decode_uint64, true},
	/* [한국어] Current Reservation Key — optional */
	{"bdev_uuid", offsetof(struct _nvmf_ns_reservation, bdev_uuid), spdk_json_decode_string},
	/* [한국어] bdev UUID — 필수 (bdev 식별, 다른 bdev로 NS가 교체되었는지 검증) */
	{"holder_uuid", offsetof(struct _nvmf_ns_reservation, holder_uuid), spdk_json_decode_string, true},
	/* [한국어] holder 호스트 UUID — optional (reservation이 없으면 누락) */
	{"registrants", offsetof(struct _nvmf_ns_reservation, regs), nvmf_decode_ns_pr_regs},
	/* [한국어] 등록자 배열 — 필수 */
};

/*
 * [한국어]
 * nvmf_ns_reservation_load_json - PTPL 파일에서 PR 상태를 JSON 파싱하여 info에 저장
 *
 * @ns:   PTPL 파일 경로(ns->ptpl_file)를 가진 namespace
 * @info: 파싱 결과를 저장할 spdk_nvmf_reservation_info 구조체
 * @return: 0=성공(파일 없으면 no-op), 음수=파싱/디코딩 오류
 *
 * PTPL 파일이 없으면 오류 없이 0 반환 (첫 실행 시 정상).
 * 파일이 있으면 전체 내용을 메모리에 로드하여 JSON 파싱 후 info에 복사.
 * 2단계 파싱 (크기 확인 → 값 파싱)으로 메모리 할당 최소화.
 *
 * 호출 체인:
 *   nvmf_ns_reservation_load → [nvmf_ns_reservation_load_json]
 */
static int
nvmf_ns_reservation_load_json(const struct spdk_nvmf_ns *ns,
			      struct spdk_nvmf_reservation_info *info)
{
	size_t json_size;			/* [한국어] 파일 크기 */
	ssize_t values_cnt, rc;			/* [한국어] JSON token 수 / 에러 코드 */
	void *json = NULL, *end;		/* [한국어] JSON 파일 버퍼 / 파싱 끝 포인터 */
	struct spdk_json_val *values = NULL;	/* [한국어] JSON token 배열 */
	struct _nvmf_ns_reservation res = {};	/* [한국어] 파싱 결과 임시 저장 구조체 (0 초기화) */
	const char *file = ns->ptpl_file;	/* [한국어] PTPL 파일 경로 */
	uint32_t i;				/* [한국어] 등록자 복사 루프 인덱스 */

	/* It's not an error if the file does not exist */
	if (access(file, F_OK) != 0) {
		SPDK_DEBUGLOG(nvmf, "File %s does not exist\n", file);
		return 0;	/* [한국어] 파일 미존재 — 첫 실행 또는 PTPL 미사용, 정상 처리 */
	}

	/* Load all persist file contents into a local buffer */
	json = spdk_posix_file_load_from_name(file, &json_size);
	/* [한국어] 파일 전체를 메모리 버퍼에 로드 (동적 할당, 호출자가 free) */
	if (!json) {
		SPDK_ERRLOG("Load persist file %s failed\n", file);
		return -ENOMEM;
	}

	rc = spdk_json_parse(json, json_size, NULL, 0, &end, 0);
	/* [한국어] 1단계 파싱: values=NULL로 token 수만 계산 */
	if (rc < 0) {
		SPDK_NOTICELOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto exit;
	}

	values_cnt = rc;			/* [한국어] 필요한 token 수 */
	values = calloc(values_cnt, sizeof(struct spdk_json_val));
	/* [한국어] token 배열 할당 */
	if (values == NULL) {
		goto exit;
	}

	rc = spdk_json_parse(json, json_size, values, values_cnt, &end, 0);
	/* [한국어] 2단계 파싱: 실제 token 값 채움 */
	if (rc != values_cnt) {
		SPDK_ERRLOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto exit;
	}

	/* Decode json */
	if (spdk_json_decode_object(values, nvmf_ns_pr_decoders,
				    SPDK_COUNTOF(nvmf_ns_pr_decoders),
				    &res)) {
		SPDK_ERRLOG("Invalid objects in the persist file %s\n", file);
		rc = -EINVAL;
		goto exit;	/* [한국어] JSON 필드 디코딩 실패 — PTPL 파일 손상 */
	}

	if (res.regs.num_regs > SPDK_NVMF_MAX_NUM_REGISTRANTS) {
		SPDK_ERRLOG("Can only support up to %u registrants\n", SPDK_NVMF_MAX_NUM_REGISTRANTS);
		rc = -ERANGE;
		goto exit;	/* [한국어] 최대 등록자 수 초과 */
	}

	rc = 0;
	info->ptpl_activated = res.ptpl_activated;	/* [한국어] PTPL 활성 여부 복사 */
	info->rtype = res.rtype;			/* [한국어] Reservation Type 복사 */
	info->crkey = res.crkey;			/* [한국어] Current Reservation Key 복사 */
	snprintf(info->bdev_uuid, sizeof(info->bdev_uuid), "%s", res.bdev_uuid);
	/* [한국어] bdev UUID 문자열 복사 (고정 크기 배열) */
	snprintf(info->holder_uuid, sizeof(info->holder_uuid), "%s", res.holder_uuid);
	/* [한국어] holder UUID 문자열 복사 (holder 없으면 빈 문자열) */
	info->num_regs = res.regs.num_regs;		/* [한국어] 등록자 수 복사 */
	for (i = 0; i < res.regs.num_regs; i++) {
		info->registrants[i].rkey = res.regs.reg[i].rkey;
		/* [한국어] 각 등록자의 RKEY 복사 */
		snprintf(info->registrants[i].host_uuid, sizeof(info->registrants[i].host_uuid), "%s",
			 res.regs.reg[i].host_uuid);
		/* [한국어] 각 등록자의 host UUID 문자열 복사 */
	}

exit:
	free(json);		/* [한국어] 파일 버퍼 해제 */
	free(values);		/* [한국어] JSON token 배열 해제 */
	free(res.bdev_uuid);	/* [한국어] 동적 할당된 bdev_uuid 문자열 해제 */
	free(res.holder_uuid);	/* [한국어] 동적 할당된 holder_uuid 문자열 해제 */
	for (i = 0; i < res.regs.num_regs; i++) {
		free(res.regs.reg[i].host_uuid);
		/* [한국어] 각 등록자의 동적 host_uuid 문자열 해제 */
	}

	return rc;
}

static bool nvmf_ns_reservation_all_registrants_type(struct spdk_nvmf_ns *ns);
/* [한국어] 전방 선언 — nvmf_ns_reservation_restore에서 holder 결정 시 사용 */

/*
 * [한국어]
 * nvmf_ns_reservation_restore - 파싱된 PR 상태를 NS에 실제 복원
 *
 * @ns:   복원 대상 namespace
 * @info: nvmf_ns_reservation_load_json으로 파싱된 PR 상태 정보
 * @return: 0=성공, -EINVAL(crkey/bdev_uuid 불일치), -ENOMEM(registrant 할당 실패)
 *
 * PTPL 파일의 PR 상태를 NS 메모리 구조체에 복원한다:
 *   1) ptpl_activated와 num_regs 확인 (비활성이거나 등록자 없으면 no-op)
 *   2) crkey가 등록자 목록에 있는지 검증
 *   3) bdev UUID를 현재 NS와 대조 (다른 bdev로 NS 재생성 시 PR 불일치 방지)
 *   4) NS의 crkey/rtype/ptpl_activated 복원
 *   5) 각 registrant를 동적 할당하여 ns->registrants TAILQ에 삽입
 *   6) holder 결정 (all_registrants_type이면 첫 번째, 아니면 holder_uuid 일치 등록자)
 *
 * 호출 체인:
 *   spdk_nvmf_subsystem_add_ns_ext → nvmf_ns_reservation_load → [nvmf_ns_reservation_restore]
 */
static int
nvmf_ns_reservation_restore(struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info)
{
	uint32_t i;					/* [한국어] 등록자 복원 루프 인덱스 */
	struct spdk_nvmf_registrant *reg, *holder = NULL;	/* [한국어] 새 등록자 / holder 등록자 */
	struct spdk_uuid bdev_uuid, holder_uuid;	/* [한국어] 파싱된 UUID 구조체 */
	bool rkey_flag = false;				/* [한국어] crkey가 등록자 목록에 있는지 플래그 */

	SPDK_DEBUGLOG(nvmf, "NSID %u, PTPL %u, Number of registrants %u\n",
		      ns->nsid, info->ptpl_activated, info->num_regs);

	/* it's not an error */
	if (!info->ptpl_activated || !info->num_regs) {
		return 0;	/* [한국어] PTPL 미활성 또는 등록자 없음 — no-op (정상) */
	}

	/* Check info->crkey exist or not in info->registrants[i].rkey */
	for (i = 0; i < info->num_regs; i++) {
		if (info->crkey == info->registrants[i].rkey) {
			rkey_flag = true;	/* [한국어] crkey가 등록자 RKEY 목록에 있음 */
		}
	}
	if (!rkey_flag && info->crkey != 0) {
		return -EINVAL;
		/* [한국어] crkey != 0인데 어떤 등록자의 RKEY에도 없음 — 상태 불일치 */
	}

	spdk_uuid_parse(&bdev_uuid, info->bdev_uuid);
	/* [한국어] PTPL 파일의 bdev UUID 문자열을 UUID 구조체로 파싱 */
	if (spdk_uuid_compare(&bdev_uuid, spdk_bdev_get_uuid(ns->bdev))) {
		SPDK_ERRLOG("Existing bdev UUID is not same with configuration file\n");
		return -EINVAL;
		/* [한국어] 현재 NS의 bdev UUID가 PTPL 파일의 것과 다름 (다른 bdev로 교체) */
	}

	ns->crkey = info->crkey;			/* [한국어] Current Reservation Key 복원 */
	ns->rtype = info->rtype;			/* [한국어] Reservation Type 복원 */
	ns->ptpl_activated = info->ptpl_activated;	/* [한국어] PTPL 활성 상태 복원 */
	spdk_uuid_parse(&holder_uuid, info->holder_uuid);
	/* [한국어] holder UUID 문자열을 UUID 구조체로 파싱 (holder 탐색 준비) */

	SPDK_DEBUGLOG(nvmf, "Bdev UUID %s\n", info->bdev_uuid);
	if (info->rtype) {
		SPDK_DEBUGLOG(nvmf, "Holder UUID %s, RTYPE %u, RKEY 0x%"PRIx64"\n",
			      info->holder_uuid, info->rtype, info->crkey);
	}

	for (i = 0; i < info->num_regs; i++) {
		/* [한국어] 각 registrant 복원 */
		reg = calloc(1, sizeof(*reg));	/* [한국어] 0 초기화 registrant 할당 */
		if (!reg) {
			return -ENOMEM;
		}
		spdk_uuid_parse(&reg->hostid, info->registrants[i].host_uuid);
		/* [한국어] host UUID 문자열 → UUID 구조체로 파싱 */
		reg->rkey = info->registrants[i].rkey;		/* [한국어] RKEY 복원 */
		TAILQ_INSERT_TAIL(&ns->registrants, reg, link);	/* [한국어] NS 등록자 목록에 추가 */
		if (info->crkey != 0 && !spdk_uuid_compare(&holder_uuid, &reg->hostid)) {
			holder = reg;
			/* [한국어] crkey != 0이고 holder UUID와 이 등록자의 hostid가 일치 — holder 지정 */
		}
		SPDK_DEBUGLOG(nvmf, "Registrant RKEY 0x%"PRIx64", Host UUID %s\n",
			      info->registrants[i].rkey, info->registrants[i].host_uuid);
	}

	if (nvmf_ns_reservation_all_registrants_type(ns)) {
		ns->holder = TAILQ_FIRST(&ns->registrants);
		/* [한국어] all_registrants 타입 (WRITE_EXCLUSIVE_ALL_REGS 등)은 첫 등록자가 holder */
	} else {
		ns->holder = holder;
		/* [한국어] 일반 타입은 holder UUID로 식별된 등록자가 holder */
	}

	return 0;
}

/*
 * [한국어]
 * nvmf_ns_json_write_cb - JSON 직렬화 데이터를 PTPL 파일에 쓰는 콜백
 *
 * @cb_ctx: PTPL 파일 경로 문자열 (char*)
 * @data:   쓸 JSON 데이터 버퍼
 * @size:   버퍼 크기
 * @return: 0=성공, -ENOENT(파일 open 실패), -1(쓰기 부분 실패)
 *
 * spdk_json_write_begin에 등록되어 JSON 직렬화 완료 시 호출된다.
 * 파일을 "w" 모드로 open하여 기존 내용을 덮어씀 (원자적 쓰기 아님).
 */
static int
nvmf_ns_json_write_cb(void *cb_ctx, const void *data, size_t size)
{
	char *file = cb_ctx;	/* [한국어] PTPL 파일 경로 */
	size_t rc;		/* [한국어] fwrite 반환값 (쓴 바이트 수) */
	FILE *fd;		/* [한국어] 파일 스트림 */

	fd = fopen(file, "w");	/* [한국어] 쓰기 모드로 open (기존 내용 덮어쓰기) */
	if (!fd) {
		SPDK_ERRLOG("Can't open file %s for write\n", file);
		return -ENOENT;	/* [한국어] 파일 open 실패 */
	}
	rc = fwrite(data, 1, size, fd);	/* [한국어] JSON 데이터 전체 쓰기 */
	fclose(fd);			/* [한국어] 파일 닫기 (flush 포함) */

	return rc == size ? 0 : -1;	/* [한국어] 부분 쓰기 발생 시 에러 */
}

/*
 * [한국어]
 * nvmf_ns_reservation_update_json - NS의 현재 PR 상태를 PTPL 파일에 JSON으로 저장
 *
 * @ns:   PTPL 파일 경로를 가진 namespace
 * @info: 저장할 PR 상태 정보
 * @return: 0=성공, -ENOMEM(JSON writer 할당 실패), spdk_json_write_end의 반환값
 *
 * PR 상태가 변경될 때마다 호출되어 PTPL 파일을 갱신한다.
 * ptpl_activated=false이면 등록자 없이 빈 JSON 객체만 기록 (파일 초기화).
 * ptpl_activated=true이면 전체 PR 상태(rtype, crkey, holder_uuid, registrants)를 JSON으로 직렬화.
 *
 * 호출 체인:
 *   nvmf_ns_reservation_update → [nvmf_ns_reservation_update_json]
 *     → spdk_json_write_* → nvmf_ns_json_write_cb (파일 쓰기)
 */
static int
nvmf_ns_reservation_update_json(const struct spdk_nvmf_ns *ns,
				const struct spdk_nvmf_reservation_info *info)
{
	const char *file = ns->ptpl_file;	/* [한국어] PTPL 파일 경로 */
	struct spdk_json_write_ctx *w;		/* [한국어] JSON writer 컨텍스트 */
	uint32_t i;				/* [한국어] 등록자 순회 인덱스 */
	int rc = 0;				/* [한국어] 에러 코드 */

	w = spdk_json_write_begin(nvmf_ns_json_write_cb, (void *)file, 0);
	/* [한국어] JSON writer 생성 (write 완료 시 nvmf_ns_json_write_cb 호출) */
	if (w == NULL) {
		return -ENOMEM;
	}
	/* clear the configuration file */
	if (!info->ptpl_activated) {
		goto exit;	/* [한국어] PTPL 비활성 — 빈 JSON으로 파일 초기화 */
	}

	spdk_json_write_object_begin(w);	/* [한국어] JSON 객체 시작 '{' */
	spdk_json_write_named_bool(w, "ptpl", info->ptpl_activated);
	/* [한국어] "ptpl": true */
	spdk_json_write_named_uint32(w, "rtype", info->rtype);
	/* [한국어] "rtype": <reservation type enum> */
	spdk_json_write_named_uint64(w, "crkey", info->crkey);
	/* [한국어] "crkey": <current reservation key> */
	spdk_json_write_named_string(w, "bdev_uuid", info->bdev_uuid);
	/* [한국어] "bdev_uuid": "<UUID 문자열>" */
	spdk_json_write_named_string(w, "holder_uuid", info->holder_uuid);
	/* [한국어] "holder_uuid": "<holder UUID>" */

	spdk_json_write_named_array_begin(w, "registrants");
	/* [한국어] "registrants": [ */
	for (i = 0; i < info->num_regs; i++) {
		spdk_json_write_object_begin(w);	/* [한국어] '{' */
		spdk_json_write_named_uint64(w, "rkey", info->registrants[i].rkey);
		/* [한국어] "rkey": <등록자 RKEY> */
		spdk_json_write_named_string(w, "host_uuid", info->registrants[i].host_uuid);
		/* [한국어] "host_uuid": "<등록자 host UUID>" */
		spdk_json_write_object_end(w);		/* [한국어] '}' */
	}
	spdk_json_write_array_end(w);		/* [한국어] ']' */
	spdk_json_write_object_end(w);		/* [한국어] 최상위 JSON 객체 닫기 '}' */

exit:
	rc = spdk_json_write_end(w);		/* [한국어] JSON 직렬화 완료 → write_cb 호출 → 파일 기록 */
	return rc;
}

/*
 * [한국어]
 * nvmf_ns_update_reservation_info - NS의 현재 PR 상태를 info로 직렬화하여 PTPL 파일 갱신
 *
 * @ns: 업데이트할 namespace
 * @return: 0=성공(PTPL 미지원이면 no-op), 음수=JSON 쓰기 오류
 *
 * NS의 현재 PR 상태(rtype, crkey, holder, registrants)를 info 구조체에 수집하고
 * nvmf_ns_reservation_update → nvmf_ns_reservation_update_json으로 PTPL 파일에 기록.
 * PR 상태가 변경될 때마다 호출된다 (REGISTER/ACQUIRE/RELEASE/PREEMPT 처리 후).
 *
 * 호출 체인:
 *   nvmf_ns_reservation_register/acquire/release → [nvmf_ns_update_reservation_info]
 *     → nvmf_ns_reservation_update → nvmf_ns_reservation_update_json → nvmf_ns_json_write_cb
 */
static int
nvmf_ns_update_reservation_info(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_reservation_info info;	/* [한국어] 직렬화할 PR 상태 정보 구조체 */
	struct spdk_nvmf_registrant *reg, *tmp;	/* [한국어] 등록자 순회 포인터 (SAFE) */
	uint32_t i = 0;				/* [한국어] 등록자 복사 인덱스 */

	assert(ns != NULL);			/* [한국어] NULL NS 방어 */

	if (!ns->bdev || !nvmf_ns_is_ptpl_capable(ns)) {
		return 0;	/* [한국어] bdev 없거나 PTPL 파일 없으면 저장 불필요 */
	}

	memset(&info, 0, sizeof(info));		/* [한국어] info 구조체 0 초기화 */
	spdk_uuid_fmt_lower(info.bdev_uuid, sizeof(info.bdev_uuid), spdk_bdev_get_uuid(ns->bdev));
	/* [한국어] bdev UUID를 소문자 UUID 문자열로 포맷 */

	if (ns->rtype) {
		/* [한국어] active reservation이 있는 경우 holder/crkey 정보 수집 */
		info.rtype = ns->rtype;		/* [한국어] Reservation Type 복사 */
		info.crkey = ns->crkey;		/* [한국어] Current Reservation Key 복사 */
		if (!nvmf_ns_reservation_all_registrants_type(ns)) {
			assert(ns->holder != NULL);
			spdk_uuid_fmt_lower(info.holder_uuid, sizeof(info.holder_uuid), &ns->holder->hostid);
			/* [한국어] all_registrants 타입이 아닌 경우에만 holder UUID 기록 */
		}
	}

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		/* [한국어] 모든 등록자를 info에 복사 (SAFE: 순회 중 변경 가능) */
		if (i < SPDK_NVMF_MAX_NUM_REGISTRANTS) {
			spdk_uuid_fmt_lower(info.registrants[i].host_uuid, sizeof(info.registrants[i].host_uuid),
					    &reg->hostid);
			/* [한국어] 등록자 host UUID를 소문자 문자열로 포맷 */
			info.registrants[i++].rkey = reg->rkey;
			/* [한국어] 등록자 RKEY 복사 및 인덱스 증가 */
		} else {
			SPDK_ERRLOG("More registrants that can fit into reservation info, truncating\n");
			/* This should never happen as we enforce SPDK_NVMF_MAX_NUM_REGISTRANTS
			 * on ns->registrants. We don't want to continue with missing registrants
			 * from the ptpl state.
			 */
			abort();	/* [한국어] 배열 초과는 버그 — abort */
		}
	}

	info.num_regs = i;			/* [한국어] 복사된 등록자 수 */
	info.ptpl_activated = ns->ptpl_activated;	/* [한국어] PTPL 활성 상태 복사 */

	return nvmf_ns_reservation_update(ns, &info);
	/* [한국어] info를 PTPL 파일에 JSON으로 기록 */
}

/*
 * [한국어]
 * nvmf_ns_registrants_get_count - NS의 현재 등록자 수 반환
 *
 * @ns: 조회할 namespace
 * @return: ns->registrants TAILQ의 엔트리 수
 *
 * SPDK_NVMF_MAX_NUM_REGISTRANTS 초과 여부 확인 등에 사용.
 */
size_t
nvmf_ns_registrants_get_count(const struct spdk_nvmf_ns *ns)
{
	size_t count = 0;			/* [한국어] 카운터 */
	struct spdk_nvmf_registrant *reg;	/* [한국어] 순회 포인터 */

	TAILQ_FOREACH(reg, &ns->registrants, link) {
		count++;	/* [한국어] 각 등록자 카운트 */
	}
	return count;
}

/*
 * [한국어]
 * nvmf_ns_reservation_get_registrant - hostid로 등록자 탐색
 *
 * @ns:   탐색할 namespace
 * @uuid: 찾을 호스트 UUID
 * @return: 일치하는 등록자, 없으면 NULL
 *
 * REGISTER/ACQUIRE/RELEASE/PREEMPT 처리 시 ctrlr의 hostid로 기존 등록 여부 확인.
 */
static struct spdk_nvmf_registrant *
nvmf_ns_reservation_get_registrant(struct spdk_nvmf_ns *ns,
				   struct spdk_uuid *uuid)
{
	struct spdk_nvmf_registrant *reg, *tmp;	/* [한국어] 순회 포인터 (SAFE) */

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		if (!spdk_uuid_compare(&reg->hostid, uuid)) {
			return reg;	/* [한국어] hostid UUID 일치 — 등록자 반환 */
		}
	}

	return NULL;	/* [한국어] 미등록 */
}

/* Generate reservation notice log to registered HostID controllers */
/*
 * [한국어]
 * nvmf_subsystem_gen_ctrlr_notification - hostid 목록에 해당하는 ctrlr들에게 PR 알림 로그 발송
 *
 * @subsystem:   대상 subsystem
 * @ns:          PR 이벤트가 발생한 namespace
 * @hostid_list: 알림을 받을 호스트 UUID 배열
 * @num_hostid:  배열 내 유효 항목 수
 * @type:        알림 로그 타입 (RESERVATION_REGISTERED/UNREGISTERED/PREEMPTED 등)
 *
 * NVMe spec: PR 상태 변경 시 관련 호스트들에게 Reservation Notification Log를 발송한다.
 * subsystem의 모든 ctrlr를 탐색하여 hostid_list에 포함된 ctrlr에게 nvmf_ctrlr_reservation_notice_log.
 */
static void
nvmf_subsystem_gen_ctrlr_notification(struct spdk_nvmf_subsystem *subsystem,
				      struct spdk_nvmf_ns *ns,
				      struct spdk_uuid *hostid_list,
				      uint32_t num_hostid,
				      enum spdk_nvme_reservation_notification_log_page_type type)
{
	struct spdk_nvmf_ctrlr *ctrlr;	/* [한국어] 순회 중인 ctrlr */
	uint32_t i;			/* [한국어] hostid_list 인덱스 */

	for (i = 0; i < num_hostid; i++) {
		/* [한국어] 각 hostid에 대해 subsystem의 모든 ctrlr 탐색 */
		TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
			if (!spdk_uuid_compare(&ctrlr->hostid, &hostid_list[i])) {
				nvmf_ctrlr_reservation_notice_log(ctrlr, ns, type);
				/* [한국어] hostid 일치 ctrlr에게 PR 알림 로그 발송 */
			}
		}
	}
}

/* Get all registrants' hostid other than the controller who issued the command */
/*
 * [한국어]
 * nvmf_ns_reservation_get_all_other_hostid - 커맨드 발송 ctrlr 제외 모든 등록자 hostid 수집
 *
 * @ns:              대상 namespace
 * @hostid_list:     수집된 hostid를 저장할 배열 (호출자가 max_num_hostid 크기로 할당)
 * @max_num_hostid:  배열 최대 크기
 * @current_hostid:  제외할 커맨드 발송 ctrlr의 hostid
 * @return: 수집된 hostid 수
 *
 * REGISTER/ACQUIRE/RELEASE 처리 후 현재 ctrlr를 제외한 다른 모든 등록자에게
 * PR 알림 로그를 발송할 때 사용.
 */
static uint32_t
nvmf_ns_reservation_get_all_other_hostid(struct spdk_nvmf_ns *ns,
		struct spdk_uuid *hostid_list,
		uint32_t max_num_hostid,
		struct spdk_uuid *current_hostid)
{
	struct spdk_nvmf_registrant *reg, *tmp;	/* [한국어] 순회 포인터 (SAFE) */
	uint32_t num_hostid = 0;		/* [한국어] 수집된 hostid 수 */

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		/* [한국어] 모든 등록자 순회 */
		if (spdk_uuid_compare(&reg->hostid, current_hostid)) {
			/* [한국어] current_hostid와 다른 등록자만 수집 */
			if (num_hostid == max_num_hostid) {
				assert(false);			/* [한국어] 배열 초과는 버그 */
				return max_num_hostid;
			}
			hostid_list[num_hostid++] = reg->hostid;
		}
	}

	return num_hostid;
}

/* Calculate the unregistered HostID list according to list
 * prior to execute preempt command and list after executing
 * preempt command.
 */
/*
 * [한국어]
 * nvmf_ns_reservation_get_unregistered_hostid - preempt 전후 등록자 목록 차이(제거된 hostid) 계산
 *
 * @old_hostid_list:        preempt 전 등록자 hostid 배열 (in/out: 결과로 덮어씀)
 * @old_num_hostid:         preempt 전 등록자 수
 * @remaining_hostid_list:  preempt 후 남은 등록자 hostid 배열
 * @remaining_num_hostid:   남은 등록자 수
 * @return: preempt로 제거된 hostid 수 (old에 있지만 remaining에 없는 것)
 *
 * PREEMPT/PREEMPT_ABORT 처리 후 제거된 등록자들에게만 알림을 보낼 때 사용.
 * 결과(제거된 목록)는 old_hostid_list에 덮어씀.
 */
static uint32_t
nvmf_ns_reservation_get_unregistered_hostid(struct spdk_uuid *old_hostid_list,
		uint32_t old_num_hostid,
		struct spdk_uuid *remaining_hostid_list,
		uint32_t remaining_num_hostid)
{
	struct spdk_uuid temp_hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	/* [한국어] 임시 결과 배열 (제거된 hostid 수집) */
	uint32_t i, j, num_hostid = 0;	/* [한국어] 루프 인덱스 / 제거된 hostid 수 */
	bool found;			/* [한국어] remaining에서 발견 여부 플래그 */

	if (!remaining_num_hostid) {
		return old_num_hostid;	/* [한국어] remaining이 없으면 old 전체가 제거됨 */
	}

	for (i = 0; i < old_num_hostid; i++) {
		/* [한국어] 각 old hostid가 remaining에 있는지 확인 */
		found = false;
		for (j = 0; j < remaining_num_hostid; j++) {
			if (!spdk_uuid_compare(&old_hostid_list[i], &remaining_hostid_list[j])) {
				found = true;	/* [한국어] remaining에 있으면 제거되지 않음 */
				break;
			}
		}
		if (!found) {
			spdk_uuid_copy(&temp_hostid_list[num_hostid++], &old_hostid_list[i]);
			/* [한국어] remaining에 없으면 제거됨 — 임시 배열에 수집 */
		}
	}

	if (num_hostid) {
		memcpy(old_hostid_list, temp_hostid_list, sizeof(struct spdk_uuid) * num_hostid);
		/* [한국어] 제거된 목록으로 old_hostid_list 덮어쓰기 (in-place) */
	}

	return num_hostid;	/* [한국어] 제거된 hostid 수 반환 */
}

/* current reservation type is all registrants or not */
/*
 * [한국어]
 * nvmf_ns_reservation_all_registrants_type - 현재 reservation이 all-registrants 타입인지 확인
 *
 * @ns: 확인할 namespace
 * @return: true=WRITE_EXCLUSIVE_ALL_REGS 또는 EXCLUSIVE_ACCESS_ALL_REGS
 *
 * all-registrants 타입은 모든 등록자가 holder로 간주되는 특수 타입.
 * 이 타입에서는 별도의 holder를 지정하지 않고 모든 등록자가 접근 가능.
 */
static bool
nvmf_ns_reservation_all_registrants_type(struct spdk_nvmf_ns *ns)
{
	return (ns->rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS ||
		ns->rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS);
	/* [한국어] NVMe spec 정의 all-registrants 타입 두 가지 */
}

/* current registrant is reservation holder or not */
/*
 * [한국어]
 * nvmf_ns_reservation_registrant_is_holder - 등록자가 현재 reservation holder인지 확인
 *
 * @ns:  확인할 namespace
 * @reg: 확인할 등록자 (NULL이면 false)
 * @return: true=holder
 *
 * all-registrants 타입이면 모든 등록자가 holder.
 * 일반 타입이면 ns->holder와 동일 포인터인지 확인.
 */
static bool
nvmf_ns_reservation_registrant_is_holder(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_registrant *reg)
{
	if (!reg) {
		return false;	/* [한국어] NULL 등록자는 holder 아님 */
	}

	if (nvmf_ns_reservation_all_registrants_type(ns)) {
		return true;	/* [한국어] all-registrants 타입 — 모두 holder */
	}

	return (ns->holder == reg);	/* [한국어] 일반 타입 — holder 포인터 비교 */
}

/*
 * [한국어]
 * nvmf_ns_reservation_add_registrant - NS에 새 registrant 추가
 *
 * @ns:    등록할 namespace
 * @ctrlr: 등록을 요청한 ctrlr (hostid와 cntlid 제공)
 * @nrkey: 새 등록자의 Reservation Key(New Reservation Key, NRKEY)
 * @return: 0=성공, -ENOMEM(등록자 수 상한 또는 메모리 할당 실패)
 *
 * REGISTER 커맨드(RREGA=REGISTER ACTION) 처리 시 새 등록자를 NS에 추가.
 * 등록 성공 시 ns->gen을 증가시켜 poll group 캐시 무효화를 알림.
 */
static int
nvmf_ns_reservation_add_registrant(struct spdk_nvmf_ns *ns,
				   struct spdk_nvmf_ctrlr *ctrlr,
				   uint64_t nrkey)
{
	struct spdk_nvmf_registrant *reg;	/* [한국어] 새 등록자 구조체 */

	if (nvmf_ns_registrants_get_count(ns) >= SPDK_NVMF_MAX_NUM_REGISTRANTS) {
		SPDK_ERRLOG("Registrant list full on subsystem: %p, nsid: %u\n", ns->subsystem, ns->nsid);
		return -ENOMEM;		/* [한국어] 등록자 수 상한 도달 */
	}

	reg = calloc(1, sizeof(*reg));	/* [한국어] 0 초기화 등록자 구조체 할당 */
	if (!reg) {
		return -ENOMEM;
	}

	reg->rkey = nrkey;		/* [한국어] 등록자 RKEY 설정 */
	reg->cntlid = ctrlr->cntlid;	/* [한국어] 등록 요청 ctrlr의 CNTLID 저장 */
	/* set hostid for the registrant */
	spdk_uuid_copy(&reg->hostid, &ctrlr->hostid);
	/* [한국어] ctrlr의 hostid UUID 복사 — 등록자 식별에 사용 */
	TAILQ_INSERT_TAIL(&ns->registrants, reg, link);	/* [한국어] 등록자 목록 말미에 추가 */
	ns->gen++;	/* [한국어] gen 증가 — poll group이 캐시를 갱신해야 함을 표시 */

	return 0;
}

/*
 * [한국어]
 * nvmf_ns_reservation_release_reservation - NS의 reservation 상태 초기화
 *
 * @ns: 대상 namespace
 *
 * holder/rtype/crkey를 모두 0/NULL로 초기화.
 * RELEASE 커맨드 처리 또는 마지막 등록자 제거 시 호출.
 */
static void
nvmf_ns_reservation_release_reservation(struct spdk_nvmf_ns *ns)
{
	ns->rtype = 0;		/* [한국어] Reservation Type 초기화 (reservation 없음) */
	ns->crkey = 0;		/* [한국어] Current Reservation Key 초기화 */
	ns->holder = NULL;	/* [한국어] holder 등록자 포인터 초기화 */
}

/* release the reservation if the last registrant was removed */
/*
 * [한국어]
 * nvmf_ns_reservation_check_release_on_remove_registrant - 등록자 제거 시 reservation 자동 해제 여부 결정
 *
 * @ns:  대상 namespace
 * @reg: 제거될 등록자
 *
 * NVMe spec: 등록자가 제거될 때 해당 등록자가 holder이면 reservation을 해제해야 한다.
 * 단, all-registrants 타입이면 다음 등록자에게 holder를 이양.
 */
static void
nvmf_ns_reservation_check_release_on_remove_registrant(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_registrant *reg)
{
	struct spdk_nvmf_registrant *next_reg;	/* [한국어] all-registrants 타입에서 다음 holder */

	/* no reservation holder */
	if (!ns->holder) {
		assert(ns->rtype == 0);		/* [한국어] holder 없으면 rtype도 0이어야 함 */
		return;				/* [한국어] reservation 없음 — nothing to do */
	}

	next_reg = TAILQ_FIRST(&ns->registrants);
	if (next_reg && nvmf_ns_reservation_all_registrants_type(ns)) {
		/* the next valid registrant is the new holder now */
		ns->holder = next_reg;
		/* [한국어] all-registrants 타입: TAILQ의 다음 등록자가 새 holder가 됨 */
	} else if (nvmf_ns_reservation_registrant_is_holder(ns, reg)) {
		/* release the reservation */
		nvmf_ns_reservation_release_reservation(ns);
		/* [한국어] 일반 타입: 제거되는 등록자가 holder이면 reservation 해제 */
	}
}

/*
 * [한국어]
 * nvmf_ns_reservation_remove_registrant - 등록자를 NS 목록에서 제거하고 메모리 해제
 *
 * @ns:  대상 namespace
 * @reg: 제거할 등록자
 *
 * TAILQ에서 제거하고, holder 제거 시 reservation 해제 여부를 확인하며,
 * 메모리를 해제하고 ns->gen을 증가시킨다.
 * gen 증가로 poll group 캐시가 갱신되어야 함을 알린다.
 */
static void
nvmf_ns_reservation_remove_registrant(struct spdk_nvmf_ns *ns,
				      struct spdk_nvmf_registrant *reg)
{
	TAILQ_REMOVE(&ns->registrants, reg, link);	/* [한국어] 등록자 TAILQ에서 제거 */
	nvmf_ns_reservation_check_release_on_remove_registrant(ns, reg);
	/* [한국어] 이 등록자가 holder이면 reservation 해제 또는 holder 이양 */
	free(reg);	/* [한국어] 등록자 구조체 메모리 해제 */
	ns->gen++;	/* [한국어] gen 증가 — poll group 캐시 무효화 알림 */
	return;
}

/*
 * [한국어]
 * nvmf_ns_reservation_remove_registrants_by_key - 특정 RKEY를 가진 모든 등록자 제거
 *
 * @ns:   대상 namespace
 * @rkey: 제거할 RKEY 값
 * @return: 제거된 등록자 수
 *
 * PREEMPT 처리 시 피해 호스트의 등록자(prkey 일치)를 일괄 제거.
 * SAFE 순회 사용.
 */
static uint32_t
nvmf_ns_reservation_remove_registrants_by_key(struct spdk_nvmf_ns *ns,
		uint64_t rkey)
{
	struct spdk_nvmf_registrant *reg, *tmp;	/* [한국어] 순회 포인터 (SAFE) */
	uint32_t count = 0;			/* [한국어] 제거된 수 카운터 */

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		if (reg->rkey == rkey) {
			nvmf_ns_reservation_remove_registrant(ns, reg);
			/* [한국어] RKEY 일치 등록자 제거 */
			count++;
		}
	}
	return count;
}

/*
 * [한국어]
 * nvmf_ns_reservation_remove_other_registrants_by_key - 특정 등록자 제외하고 RKEY 일치 등록자 제거
 *
 * @ns:   대상 namespace
 * @rkey: 제거할 RKEY 값
 * @reg:  제외할 등록자 (PREEMPT 커맨드 발송 ctrlr의 등록자)
 *
 * PREEMPT 처리에서 prkey와 같은 RKEY를 가진 다른 등록자는 제거하되,
 * 커맨드 발송 ctrlr 자신의 등록자는 유지.
 */
static void
nvmf_ns_reservation_remove_other_registrants_by_key(struct spdk_nvmf_ns *ns,
		uint64_t rkey, const struct spdk_nvmf_registrant *reg)
{
	struct spdk_nvmf_registrant *reg_tmp, *reg_tmp2;	/* [한국어] SAFE 순회 포인터 */

	TAILQ_FOREACH_SAFE(reg_tmp, &ns->registrants, link, reg_tmp2) {
		if (reg_tmp->rkey == rkey && reg != reg_tmp) {
			/* [한국어] RKEY 일치이고 현재 ctrlr의 등록자가 아닌 것만 제거 */
			nvmf_ns_reservation_remove_registrant(ns, reg_tmp);
		}
	}
}

/*
 * [한국어]
 * nvmf_ns_reservation_remove_all_other_registrants - 특정 등록자 제외 모든 등록자 제거
 *
 * @ns:  대상 namespace
 * @reg: 유지할 등록자 (PREEMPT 커맨드 발송 ctrlr의 등록자)
 * @return: 제거된 등록자 수
 *
 * all-registrants 타입에서 PREEMPT(prkey=0) 시 커맨드 발송 ctrlr만 남기고
 * 나머지 모든 등록자를 제거.
 */
static uint32_t
nvmf_ns_reservation_remove_all_other_registrants(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_registrant *reg)
{
	struct spdk_nvmf_registrant *reg_tmp, *reg_tmp2;	/* [한국어] SAFE 순회 포인터 */
	uint32_t count = 0;					/* [한국어] 제거된 수 카운터 */

	TAILQ_FOREACH_SAFE(reg_tmp, &ns->registrants, link, reg_tmp2) {
		if (reg_tmp != reg) {
			nvmf_ns_reservation_remove_registrant(ns, reg_tmp);
			/* [한국어] reg를 제외한 모든 등록자 제거 */
			count++;
		}
	}
	return count;
}

/*
 * [한국어]
 * nvmf_ns_reservation_clear_all_registrants - NS의 모든 등록자 제거
 *
 * @ns: 대상 namespace
 * @return: 제거된 등록자 수
 *
 * NS 삭제 또는 transport 에러 시 등록자 목록 완전 초기화.
 * reservation도 자동 해제.
 */
static uint32_t
nvmf_ns_reservation_clear_all_registrants(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_registrant *reg, *reg_tmp;	/* [한국어] SAFE 순회 포인터 */
	uint32_t count = 0;				/* [한국어] 제거된 수 카운터 */

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, reg_tmp) {
		nvmf_ns_reservation_remove_registrant(ns, reg);	/* [한국어] 모든 등록자 순차 제거 */
		count++;
	}
	return count;
}

/*
 * [한국어]
 * nvmf_ns_reservation_acquire_reservation - NS reservation 설정 (ACQUIRE 처리)
 *
 * @ns:     대상 namespace
 * @rkey:   Current Reservation Key (acquire 커맨드의 crkey)
 * @rtype:  Reservation Type
 * @holder: reservation holder가 될 등록자
 *
 * ns->holder가 NULL인 상태 (첫 acquisition)에서만 호출된다 (assert 검증).
 * reservation 상태(rtype, crkey, holder)를 한 번에 설정.
 */
static void
nvmf_ns_reservation_acquire_reservation(struct spdk_nvmf_ns *ns, uint64_t rkey,
					enum spdk_nvme_reservation_type rtype,
					struct spdk_nvmf_registrant *holder)
{
	ns->rtype = rtype;			/* [한국어] Reservation Type 설정 */
	ns->crkey = rkey;			/* [한국어] Current Reservation Key 설정 */
	assert(ns->holder == NULL);		/* [한국어] 첫 acquisition이므로 holder가 없어야 함 */
	ns->holder = holder;			/* [한국어] holder 등록자 지정 */
}

/*
 * [한국어]
 * nvmf_ns_reservation_register - NVMe Reservation Register 커맨드 처리
 *
 * @ns:   대상 namespace
 * @ctrlr: 커맨드를 발송한 ctrlr (hostid로 등록자 식별)
 * @req:  원본 NVMe 요청 (cdw10에서 RREGA/IEKEY/CPTPL, 데이터 버퍼에서 CRKEY/NRKEY)
 * @return: poll group 캐시 갱신 필요 여부 (true = 변경 발생)
 *
 * NVMe spec §8.19.6.1: Reservation Register 커맨드는 세 가지 동작을 선택한다.
 *   - REGISTER_KEY(0): 새 등록자 추가 (NRKEY != 0)
 *   - UNREGISTER_KEY(1): 기존 등록자 제거 (CRKEY 일치 필요, IEKEY=1이면 키 무시)
 *   - REPLACE_KEY(2): 기존 RKEY를 새 NRKEY로 교체
 *
 * CPTPL(Persist Through Power Loss) 처리:
 *   - CLEAR(1): ptpl_activated = 0, JSON 파일 업데이트
 *   - PERSIST(3): ptpl_activated = 1, JSON 파일 업데이트 (nvmf_ns_is_ptpl_capable 필수)
 *
 * 실행 컨텍스트: subsystem->thread. nvmf_ns_reservation_update_state에서 직렬화된 후 호출.
 *
 * 호출 체인:
 *   nvmf_ns_reservation_update_state → [nvmf_ns_reservation_register]
 *     → nvmf_ns_reservation_add_registrant
 *     → nvmf_ns_reservation_remove_registrant
 *     → nvmf_subsystem_gen_ctrlr_notification (RELEASE 시)
 */
static bool
nvmf_ns_reservation_register(struct spdk_nvmf_ns *ns,
			     struct spdk_nvmf_ctrlr *ctrlr,
			     struct spdk_nvmf_request *req)
{
	struct spdk_nvme_reservation_register_data key = { 0 };
	/* [한국어] REGISTER 커맨드의 데이터 버퍼: CRKEY(현재 키)와 NRKEY(새 키) */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	/* [한국어] NVMe 커맨드: cdw10_bits.resv_register에서 RREGA/IEKEY/CPTPL 추출 */
	uint8_t rrega, iekey, cptpl, rtype;
	/* [한국어] rrega: Register Action, iekey: Ignore Existing Key, cptpl: PTPL 제어 */
	struct spdk_nvmf_registrant *reg;	/* [한국어] 현재 hostid의 기존 등록자 (없으면 NULL) */
	uint8_t status = SPDK_NVME_SC_SUCCESS;	/* [한국어] 응답 status code */
	bool update_sgroup = false;		/* [한국어] poll group 캐시 갱신 필요 플래그 */
	struct spdk_uuid hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	/* [한국어] UNREGISTER 시 AEN 발송 대상 hostid 목록 */
	uint32_t num_hostid = 0;		/* [한국어] hostid_list의 유효 원소 수 */
	int rc;					/* [한국어] add_registrant 반환 코드 */

	rrega = cmd->cdw10_bits.resv_register.rrega;
	/* [한국어] Register Action 추출: 0=REGISTER, 1=UNREGISTER, 2=REPLACE */
	iekey = cmd->cdw10_bits.resv_register.iekey;
	/* [한국어] Ignore Existing Key 비트: 1이면 CRKEY 일치 검사 생략 */
	cptpl = cmd->cdw10_bits.resv_register.cptpl;
	/* [한국어] CPTPL 비트: 0=변경없음, 1=CLEAR, 3=PERSIST */

	if (req->iovcnt > 0 && req->length >= sizeof(key)) {
		/* [한국어] 데이터 버퍼가 충분히 있으면 CRKEY/NRKEY 읽기 */
		struct spdk_iov_xfer ix;
		spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);	/* [한국어] iov 커서 초기화 */
		spdk_iov_xfer_to_buf(&ix, &key, sizeof(key));		/* [한국어] 데이터 버퍼 → key 구조체 복사 */
	} else {
		SPDK_ERRLOG("No key provided. Failing request.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;	/* [한국어] 데이터 없으면 INVALID_FIELD 에러 */
	}

	SPDK_DEBUGLOG(nvmf, "REGISTER: RREGA %u, IEKEY %u, CPTPL %u, "
		      "NRKEY 0x%"PRIx64", NRKEY 0x%"PRIx64"\n",
		      rrega, iekey, cptpl, key.crkey, key.nrkey);

	if (cptpl == SPDK_NVME_RESERVE_PTPL_CLEAR_POWER_ON) {
		/* True to OFF state, and need to be updated in the configuration file */
		/* [한국어] PTPL을 OFF로 변경 — ptpl_activated가 이미 0이면 no-op */
		if (ns->ptpl_activated) {
			ns->ptpl_activated = 0;		/* [한국어] PTPL 비활성화 */
			update_sgroup = true;		/* [한국어] JSON 파일 업데이트 필요 */
		}
	} else if (cptpl == SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS) {
		/* [한국어] PTPL을 ON으로 변경 — JSON 파일 백엔드 필요 */
		if (!nvmf_ns_is_ptpl_capable(ns)) {
			/* [한국어] ptpl_file이 없으면 PTPL 미지원 → INVALID_FIELD */
			status = SPDK_NVME_SC_INVALID_FIELD;
			goto exit;
		} else if (ns->ptpl_activated == 0) {
			ns->ptpl_activated = 1;		/* [한국어] PTPL 활성화 */
			update_sgroup = true;		/* [한국어] JSON 파일 업데이트 필요 */
		}
	}

	/* current Host Identifier has registrant or not */
	reg = nvmf_ns_reservation_get_registrant(ns, &ctrlr->hostid);
	/* [한국어] 발송 ctrlr의 hostid로 기존 등록자를 조회 */

	switch (rrega) {
	case SPDK_NVME_RESERVE_REGISTER_KEY:
		/* [한국어] 새 RKEY 등록 */
		if (!reg) {
			/* register new controller */
			/* [한국어] 등록자 없음 — 신규 등록 */
			if (key.nrkey == 0) {
				/* [한국어] NVMe spec: NRKEY=0은 허용되지 않음 */
				SPDK_ERRLOG("Can't register zeroed new key\n");
				status = SPDK_NVME_SC_INVALID_FIELD;
				goto exit;
			}
			rc = nvmf_ns_reservation_add_registrant(ns, ctrlr, key.nrkey);
			if (rc < 0) {
				/* [한국어] 메모리 할당 실패 */
				status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				goto exit;
			}
			update_sgroup = true;	/* [한국어] 등록자 추가됨 — poll group 갱신 */
		} else {
			/* register with same key is not an error */
			/* [한국어] 이미 등록됨 — NRKEY 불일치 시 conflict */
			if (reg->rkey != key.nrkey) {
				SPDK_ERRLOG("The same host already register a "
					    "key with 0x%"PRIx64"\n",
					    reg->rkey);
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				goto exit;
			}
			/* [한국어] 동일 NRKEY 재등록 — no-op, update_sgroup 유지 false */
		}
		break;
	case SPDK_NVME_RESERVE_UNREGISTER_KEY:
		/* [한국어] 기존 등록자 제거 */
		if (!reg || (!iekey && reg->rkey != key.crkey)) {
			/* [한국어] 등록자 없거나 CRKEY 불일치 (IEKEY=0) → conflict */
			SPDK_ERRLOG("No registrant or current key doesn't match "
				    "with existing registrant key\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}

		rtype = ns->rtype;	/* [한국어] 제거 전 reservation type 저장 (AEN 판단용) */
		num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, hostid_list,
				SPDK_NVMF_MAX_NUM_REGISTRANTS,
				&ctrlr->hostid);
		/* [한국어] 제거 전 다른 hostid 목록 저장 (AEN 발송용) */

		nvmf_ns_reservation_remove_registrant(ns, reg);
		/* [한국어] 등록자 제거 (holder이면 reservation도 해제) */

		if (!ns->rtype && num_hostid && (rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY ||
						 rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY)) {
			/* [한국어] REG_ONLY 타입에서 reservation이 해제됐으면 나머지 등록자에게 AEN 발송 */
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_RESERVATION_RELEASED);
		}
		update_sgroup = true;	/* [한국어] 등록자 제거됨 — poll group 갱신 */
		break;
	case SPDK_NVME_RESERVE_REPLACE_KEY:
		/* [한국어] 기존 RKEY를 새 NRKEY로 교체 */
		if (key.nrkey == 0) {
			SPDK_ERRLOG("Can't register zeroed new key\n");
			status = SPDK_NVME_SC_INVALID_FIELD;
			goto exit;
		}
		/* Registrant exists */
		if (reg) {
			/* [한국어] 기존 등록자 있음 */
			if (!iekey && reg->rkey != key.crkey) {
				/* [한국어] IEKEY=0이고 CRKEY 불일치 → conflict */
				SPDK_ERRLOG("Current key doesn't match "
					    "existing registrant key\n");
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				goto exit;
			}
			if (reg->rkey == key.nrkey) {
				/* [한국어] 동일 키로 교체 — no-op */
				goto exit;
			}
			reg->rkey = key.nrkey;	/* [한국어] 등록자의 RKEY를 새 키로 교체 */
			if (nvmf_ns_reservation_registrant_is_holder(ns, reg)) {
				/* [한국어] 이 등록자가 holder이면 ns->crkey도 동기화 */
				ns->crkey = key.nrkey;
			}
		} else if (iekey) { /* No registrant but IEKEY is set */
			/* new registrant */
			/* [한국어] 등록자 없고 IEKEY=1 → 신규 등록자로 추가 */
			rc = nvmf_ns_reservation_add_registrant(ns, ctrlr, key.nrkey);
			if (rc < 0) {
				status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				goto exit;
			}
		} else { /* No registrant */
			/* [한국어] 등록자 없고 IEKEY=0 → conflict */
			SPDK_ERRLOG("No registrant\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;

		}
		update_sgroup = true;	/* [한국어] 키 교체됨 — poll group 갱신 */
		break;
	default:
		/* [한국어] 알 수 없는 RREGA 값 */
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	/* [한국어] status type: Generic Command Status */
	req->rsp->nvme_cpl.status.sc = status;	/* [한국어] status code 설정 */
	return update_sgroup;			/* [한국어] 상태 변경 여부 반환 */
}

/*
 * [한국어]
 * nvmf_ns_reservation_acquire - NVMe Reservation Acquire 커맨드 처리
 *
 * @ns:   대상 namespace
 * @ctrlr: 커맨드를 발송한 ctrlr
 * @req:  원본 NVMe 요청 (cdw10에서 RACQA/IEKEY/RTYPE, 데이터에서 CRKEY/PRKEY)
 * @return: poll group 캐시 갱신 필요 여부 (에러 시 false 반환 가능)
 *
 * NVMe spec §8.19.6.2: Reservation Acquire 커맨드는 세 가지 동작을 선택한다.
 *   - ACQUIRE(0): holder가 없을 때 등록자가 reservation 획득
 *   - PREEMPT(1): 다른 등록자(prkey 일치)의 reservation을 탈취
 *   - PREEMPT_ABORT(2): PREEMPT + 해당 등록자의 미완료 IO abort
 *
 * all_registrants 타입(WRITE_EXCLUSIVE_ALL_REGS, EXCLUSIVE_ACCESS_ALL_REGS)에서는
 * prkey=0 이면 모든 다른 등록자를 제거하고 본 ctrlr만 남긴다.
 *
 * PREEMPT_ABORT 처리:
 *   ns->preempt_abort 구조체를 할당하고 제거된 hostid 목록을 저장.
 *   이후 ns_reservation_sched_next_io_wait_check로 IO 대기 완료를 polling.
 *
 * 실행 컨텍스트: subsystem->thread. nvmf_ns_reservation_update_state에서 호출.
 *
 * 호출 체인:
 *   nvmf_ns_reservation_update_state → [nvmf_ns_reservation_acquire]
 *     → nvmf_ns_reservation_acquire_reservation
 *     → nvmf_ns_reservation_remove_registrants_by_key / remove_other_... / remove_all_...
 *     → nvmf_subsystem_gen_ctrlr_notification (AEN 발송)
 */
static bool
nvmf_ns_reservation_acquire(struct spdk_nvmf_ns *ns,
			    struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_reservation_acquire_data key = { 0 };
	/* [한국어] ACQUIRE 커맨드의 데이터 버퍼: CRKEY(현재 등록자 키)와 PRKEY(preempt 대상 키) */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	/* [한국어] NVMe 커맨드: cdw10에서 RACQA/IEKEY/RTYPE 추출 */
	uint8_t racqa, iekey, rtype;
	/* [한국어] racqa: Reservation Acquire Action, iekey: Ignore Existing Key, rtype: Reservation Type */
	struct spdk_nvmf_registrant *reg;	/* [한국어] 발송 ctrlr의 기존 등록자 */
	bool all_regs = false;			/* [한국어] all-registrants 타입 여부 */
	uint32_t count = 0;			/* [한국어] PREEMPT에서 제거된 등록자 수 */
	bool update_sgroup = true;		/* [한국어] poll group 캐시 갱신 필요 플래그 */
	struct spdk_uuid hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	/* [한국어] PREEMPT 전 다른 hostid 목록 (AEN 발송용 delta 계산) */
	uint32_t num_hostid = 0;		/* [한국어] hostid_list의 유효 원소 수 */
	struct spdk_uuid new_hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	/* [한국어] PREEMPT 후 남은 다른 hostid 목록 (RESERVATION_RELEASED AEN 발송용) */
	uint32_t new_num_hostid = 0;		/* [한국어] new_hostid_list의 유효 원소 수 */
	bool reservation_released = false;	/* [한국어] reservation 이전(preempt) 발생 여부 */
	bool is_preempt = false;		/* [한국어] PREEMPT or PREEMPT_ABORT 여부 */
	bool is_abort = false;			/* [한국어] PREEMPT_ABORT 여부 */
	uint8_t status = SPDK_NVME_SC_SUCCESS;	/* [한국어] 응답 status code */

	racqa = cmd->cdw10_bits.resv_acquire.racqa;
	/* [한국어] Reservation Acquire Action: 0=ACQUIRE, 1=PREEMPT, 2=PREEMPT_ABORT */
	iekey = cmd->cdw10_bits.resv_acquire.iekey;
	/* [한국어] ACQUIRE에서는 iekey 사용 금지 (spec §8.19.6.2) */
	rtype = cmd->cdw10_bits.resv_acquire.rtype;
	/* [한국어] 요청하는 Reservation Type (6가지 중 하나) */

	if (req->iovcnt > 0 && req->length >= sizeof(key)) {
		/* [한국어] 데이터 버퍼가 있으면 CRKEY/PRKEY 읽기 */
		struct spdk_iov_xfer ix;
		spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);	/* [한국어] iov 커서 초기화 */
		spdk_iov_xfer_to_buf(&ix, &key, sizeof(key));		/* [한국어] 데이터 → key 복사 */
	} else {
		SPDK_ERRLOG("No key provided. Failing request.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;	/* [한국어] 데이터 없으면 INVALID_FIELD */
	}

	SPDK_DEBUGLOG(nvmf, "ACQUIRE: RACQA %u, IEKEY %u, RTYPE %u, "
		      "NRKEY 0x%"PRIx64", PRKEY 0x%"PRIx64"\n",
		      racqa, iekey, rtype, key.crkey, key.prkey);

	if (iekey || rtype > SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS) {
		/* [한국어] ACQUIRE 커맨드에서 IEKEY=1은 허용되지 않음; rtype 범위 확인 */
		SPDK_ERRLOG("Ignore existing key field set to 1\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

	reg = nvmf_ns_reservation_get_registrant(ns, &ctrlr->hostid);
	/* must be registrant and CRKEY must match */
	/* [한국어] ACQUIRE 커맨드 발송자는 반드시 기존 등록자여야 하며 CRKEY도 일치해야 함 */
	if (!reg || reg->rkey != key.crkey) {
		SPDK_ERRLOG("No registrant or current key doesn't match "
			    "with existing registrant key\n");
		status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		update_sgroup = false;
		goto exit;	/* [한국어] 미등록자 또는 CRKEY 불일치 → RESERVATION_CONFLICT */
	}

	all_regs = nvmf_ns_reservation_all_registrants_type(ns);
	/* [한국어] all-registrants 타입 여부 확인 (PREEMPT 동작 분기에 사용) */

	switch (racqa) {
	case SPDK_NVME_RESERVE_ACQUIRE:
		/* [한국어] 일반 ACQUIRE: holder 없을 때만 획득 가능 */
		/* it's not an error for the holder to acquire same reservation type again */
		if (nvmf_ns_reservation_registrant_is_holder(ns, reg) && ns->rtype == rtype) {
			/* do nothing */
			/* [한국어] 이미 holder이고 같은 rtype — 멱등(idempotent) 처리 */
			update_sgroup = false;
		} else if (ns->holder == NULL) {
			/* first time to acquire the reservation */
			/* [한국어] holder 없음 — 첫 acquisition */
			nvmf_ns_reservation_acquire_reservation(ns, key.crkey, rtype, reg);
		} else {
			/* [한국어] holder가 있고 이 ctrlr이 holder가 아님 → conflict */
			SPDK_ERRLOG("Invalid rtype or current registrant is not holder\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			update_sgroup = false;
			goto exit;
		}
		break;
	case SPDK_NVME_RESERVE_PREEMPT:
	case SPDK_NVME_RESERVE_PREEMPT_ABORT:
		/* [한국어] PREEMPT: 다른 holder(prkey 일치)의 reservation 탈취 */
		is_preempt = true;					/* [한국어] PREEMPT 플래그 설정 */
		is_abort = (racqa == SPDK_NVME_RESERVE_PREEMPT_ABORT);	/* [한국어] ABORT 여부 */

		/* Allocate memory for performing preempt-and-abort on first abort received */
		/* [한국어] PREEMPT_ABORT 시 ns->preempt_abort 구조체 초기화 (최초 1회) */
		if (is_abort && !ns->preempt_abort) {
			ns->preempt_abort = calloc(1, sizeof(*ns->preempt_abort));
			/* [한국어] PREEMPT_ABORT 정보 구조체 할당 */
			if (!ns->preempt_abort) {
				status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				update_sgroup = false;
				goto exit;	/* [한국어] 메모리 부족 */
			}
		}

		/* Build copy of current other hosts so we can generate a delta
		 * of registrants removed due to the prempt.
		 */
		/* [한국어] PREEMPT 전 다른 hostid 목록 저장 — 나중에 제거된 것만 AEN 발송 */
		num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, hostid_list,
				SPDK_NVMF_MAX_NUM_REGISTRANTS,
				&ctrlr->hostid);

		/* no reservation holder */
		if (!ns->holder) {
			/* unregister with PRKEY */
			/* [한국어] holder 없음 — prkey 일치 등록자 제거만 수행 */
			nvmf_ns_reservation_remove_registrants_by_key(ns, key.prkey);
			break;
		}

		/* only 1 reservation holder and reservation key is valid */
		if (!all_regs) {
			/* [한국어] 단일 holder 타입 */
			/* preempt itself */
			if (nvmf_ns_reservation_registrant_is_holder(ns, reg) &&
			    ns->crkey == key.prkey) {
				/* [한국어] 자기 자신 preempt — rtype만 변경 */
				ns->rtype = rtype;
				reservation_released = true;
				break;
			}

			if (ns->crkey == key.prkey) {
				/* [한국어] 현재 holder의 CRKEY와 PRKEY 일치 — holder 탈취 */
				nvmf_ns_reservation_remove_other_registrants_by_key(ns, key.prkey, reg);
				/* [한국어] prkey 가진 다른 등록자 제거 (발송자 본인 제외) */
				nvmf_ns_reservation_acquire_reservation(ns, key.crkey, rtype, reg);
				/* [한국어] 발송자가 새 holder */
				reservation_released = true;	/* [한국어] 기존 reservation 해제됨 */
			} else if (key.prkey != 0) {
				/* [한국어] prkey 일치하지 않아도 prkey != 0이면 해당 등록자 제거 */
				nvmf_ns_reservation_remove_registrants_by_key(ns, key.prkey);
			} else {
				/* PRKEY is zero */
				/* [한국어] all_regs 아닌데 prkey=0 → conflict */
				SPDK_ERRLOG("Current PRKEY is zero\n");
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				update_sgroup = false;
				goto exit;
			}
		} else {
			/* release all other registrants except for the current one */
			/* [한국어] all-registrants 타입 */
			if (key.prkey == 0) {
				/* [한국어] prkey=0: 모든 다른 등록자 제거 */
				nvmf_ns_reservation_remove_all_other_registrants(ns, reg);
				assert(ns->holder == reg);	/* [한국어] 제거 후 발송자가 holder여야 함 */
			} else {
				/* [한국어] prkey!=0: prkey 일치 등록자만 제거 */
				count = nvmf_ns_reservation_remove_registrants_by_key(ns, key.prkey);
				if (count == 0) {
					/* [한국어] prkey 일치하는 등록자 없음 → conflict */
					SPDK_ERRLOG("PRKEY doesn't match any registrant\n");
					status = SPDK_NVME_SC_RESERVATION_CONFLICT;
					update_sgroup = false;
					goto exit;
				}
			}
		}
		break;
	default:
		/* [한국어] 알 수 없는 RACQA 값 */
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		break;
	}

exit:
	if (update_sgroup && is_preempt) {
		/* [한국어] PREEMPT가 성공적으로 수행됐으면 AEN 발송 */
		new_num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, new_hostid_list,
				 SPDK_NVMF_MAX_NUM_REGISTRANTS,
				 &ctrlr->hostid);
		/* [한국어] PREEMPT 후 남은 다른 hostid 목록 */
		/* Preempt notification occurs on the unregistered controllers
		 * other than the controller who issued the command.
		 */
		num_hostid = nvmf_ns_reservation_get_unregistered_hostid(hostid_list,
				num_hostid,
				new_hostid_list,
				new_num_hostid);
		/* [한국어] 제거된(preempt된) hostid만 남기도록 필터링 */
		if (num_hostid) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_REGISTRATION_PREEMPTED);
			/* [한국어] 제거된 등록자들에게 REGISTRATION_PREEMPTED AEN 발송 */
		}
		/* Reservation released notification occurs on the
		 * controllers which are the remaining registrants other than
		 * the controller who issued the command.
		 */
		if (reservation_released && new_num_hostid) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      new_hostid_list,
							      new_num_hostid,
							      SPDK_NVME_RESERVATION_RELEASED);
			/* [한국어] 남은 다른 등록자들에게 RESERVATION_RELEASED AEN 발송 */
		}

		/* For Preempt-and-abort copy the hostids for evaluation
		 * of outstanding IO on those controllers on each poll group */
		if (is_abort) {
			/* [한국어] PREEMPT_ABORT: 제거된 hostid 목록을 preempt_abort에 저장 */
			struct spdk_nvmf_reservation_preempt_abort_info *p_info = ns->preempt_abort;
			assert(num_hostid <= SPDK_NVMF_MAX_NUM_REGISTRANTS);
			memcpy(p_info->hostids, hostid_list,
			       sizeof(struct spdk_uuid) * num_hostid);
			/* [한국어] 제거된 hostid 배열 복사 — poll group 검사 시 사용 */
			p_info->hostids_cnt = (uint8_t)num_hostid;	/* [한국어] 유효 hostid 수 */
			p_info->hostids_gen++;		/* [한국어] gen 증가로 poll group 캐시 무효화 */
			p_info->io_waiting_done = false;	/* [한국어] IO 대기 미완료 상태로 초기화 */
			p_info->io_waiting_timeout_ticks = 0;	/* [한국어] 첫 check 시 timeout 계산 */
		}
	}
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	/* [한국어] Generic Command Status type */
	req->rsp->nvme_cpl.status.sc = status;	/* [한국어] status code 설정 */
	return update_sgroup;			/* [한국어] 상태 변경 여부 반환 */
}

/*
 * [한국어]
 * nvmf_ns_reservation_release - NVMe Reservation Release 커맨드 처리
 *
 * @ns:   대상 namespace
 * @ctrlr: 커맨드를 발송한 ctrlr
 * @req:  원본 NVMe 요청 (cdw10에서 RRELA/IEKEY/RTYPE, 데이터에서 CRKEY)
 * @return: poll group 캐시 갱신 필요 여부
 *
 * NVMe spec §8.19.6.3: Reservation Release 커맨드는 두 가지 동작을 선택한다.
 *   - RELEASE(0): 발송자가 holder인 경우 reservation 해제 (등록은 유지)
 *   - CLEAR(1): 모든 등록자 및 reservation 완전 초기화
 *
 * RELEASE 시: WE/EA 이외의 타입(REG_ONLY 계열)에서 다른 등록자에게 AEN 발송.
 * CLEAR 시: 모든 등록자에게 RESERVATION_PREEMPTED AEN 발송.
 *
 * 발송자는 반드시 등록자여야 하고 CRKEY가 일치해야 한다.
 * IEKEY=1은 허용되지 않는다 (spec §8.19.6.3).
 *
 * 실행 컨텍스트: subsystem->thread. nvmf_ns_reservation_update_state에서 호출.
 *
 * 호출 체인:
 *   nvmf_ns_reservation_update_state → [nvmf_ns_reservation_release]
 *     → nvmf_ns_reservation_release_reservation
 *     → nvmf_ns_reservation_clear_all_registrants
 *     → nvmf_subsystem_gen_ctrlr_notification
 */
static bool
nvmf_ns_reservation_release(struct spdk_nvmf_ns *ns,
			    struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	/* [한국어] NVMe 커맨드: cdw10에서 RRELA/IEKEY/RTYPE 추출 */
	uint8_t rrela, iekey, rtype;
	/* [한국어] rrela: Release Action (0=RELEASE, 1=CLEAR), iekey: 금지 비트, rtype: 확인용 */
	struct spdk_nvmf_registrant *reg;	/* [한국어] 발송 ctrlr의 기존 등록자 */
	uint64_t crkey = 0;			/* [한국어] 데이터 버퍼에서 읽은 Current Reservation Key */
	uint8_t status = SPDK_NVME_SC_SUCCESS;	/* [한국어] 응답 status code */
	bool update_sgroup = true;		/* [한국어] poll group 캐시 갱신 필요 플래그 */
	struct spdk_uuid hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	/* [한국어] AEN 발송 대상 hostid 목록 (해제 전 다른 등록자들) */
	uint32_t num_hostid = 0;		/* [한국어] hostid_list의 유효 원소 수 */

	rrela = cmd->cdw10_bits.resv_release.rrela;	/* [한국어] Release Action 추출 */
	iekey = cmd->cdw10_bits.resv_release.iekey;	/* [한국어] Ignore Existing Key 비트 (RELEASE에선 금지) */
	rtype = cmd->cdw10_bits.resv_release.rtype;	/* [한국어] Reservation Type (RELEASE 시 일치 확인용) */

	if (req->iovcnt > 0 && req->length >= sizeof(crkey)) {
		/* [한국어] 데이터 버퍼에서 CRKEY 읽기 */
		struct spdk_iov_xfer ix;
		spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);	/* [한국어] iov 커서 초기화 */
		spdk_iov_xfer_to_buf(&ix, &crkey, sizeof(crkey));	/* [한국어] 데이터 → crkey 복사 */
	} else {
		SPDK_ERRLOG("No key provided. Failing request.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;	/* [한국어] 데이터 없으면 INVALID_FIELD */
	}

	SPDK_DEBUGLOG(nvmf, "RELEASE: RRELA %u, IEKEY %u, RTYPE %u, "
		      "CRKEY 0x%"PRIx64"\n",  rrela, iekey, rtype, crkey);

	if (iekey) {
		/* [한국어] RELEASE 커맨드에서 IEKEY=1은 spec 위반 */
		SPDK_ERRLOG("Ignore existing key field set to 1\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

	reg = nvmf_ns_reservation_get_registrant(ns, &ctrlr->hostid);
	/* [한국어] 발송 ctrlr의 등록자 조회 */
	if (!reg || reg->rkey != crkey) {
		/* [한국어] 미등록자 또는 CRKEY 불일치 → RESERVATION_CONFLICT */
		SPDK_ERRLOG("No registrant or current key doesn't match "
			    "with existing registrant key\n");
		status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		update_sgroup = false;
		goto exit;
	}

	num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, hostid_list,
			SPDK_NVMF_MAX_NUM_REGISTRANTS,
			&ctrlr->hostid);
	/* [한국어] 해제 전 다른 hostid 목록 저장 (AEN 발송용) */

	switch (rrela) {
	case SPDK_NVME_RESERVE_RELEASE:
		/* [한국어] RELEASE: holder이면 reservation 해제 (등록자는 유지) */
		if (!ns->holder) {
			/* [한국어] holder 없음 — no-op */
			SPDK_DEBUGLOG(nvmf, "RELEASE: no holder\n");
			update_sgroup = false;
			goto exit;
		}
		if (ns->rtype != rtype) {
			/* [한국어] 커맨드의 rtype이 현재 reservation type과 다름 → INVALID_FIELD */
			SPDK_ERRLOG("Type doesn't match\n");
			status = SPDK_NVME_SC_INVALID_FIELD;
			update_sgroup = false;
			goto exit;
		}
		if (!nvmf_ns_reservation_registrant_is_holder(ns, reg)) {
			/* not the reservation holder, this isn't an error */
			/* [한국어] 발송자가 holder가 아닌 경우 — 에러 아님, no-op */
			update_sgroup = false;
			goto exit;
		}

		rtype = ns->rtype;		/* [한국어] AEN 판단 위해 현재 rtype 저장 */
		nvmf_ns_reservation_release_reservation(ns);
		/* [한국어] reservation 해제: ns->holder = NULL, ns->rtype = 0, ns->crkey = 0 */

		if (num_hostid && rtype != SPDK_NVME_RESERVE_WRITE_EXCLUSIVE &&
		    rtype != SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS) {
			/* [한국어] WE/EA 이외의 타입(REG_ONLY 계열)에서만 AEN 발송
			 * WE/EA는 등록자 존재를 공유하지 않으므로 통지 불필요 */
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_RESERVATION_RELEASED);
		}
		break;
	case SPDK_NVME_RESERVE_CLEAR:
		/* [한국어] CLEAR: 모든 등록자 및 reservation 완전 초기화 */
		nvmf_ns_reservation_clear_all_registrants(ns);
		/* [한국어] 모든 등록자 제거 (reservation도 자동 해제) */
		if (num_hostid) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_RESERVATION_PREEMPTED);
			/* [한국어] 제거된 등록자들에게 RESERVATION_PREEMPTED AEN 발송 */
		}
		break;
	default:
		/* [한국어] 알 수 없는 RRELA 값 */
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	/* [한국어] Generic Command Status type */
	req->rsp->nvme_cpl.status.sc = status;	/* [한국어] status code 설정 */
	return update_sgroup;			/* [한국어] 상태 변경 여부 반환 */
}

/*
 * [한국어]
 * nvmf_ns_reservation_report - NVMe Reservation Report 커맨드 처리 (읽기 전용)
 *
 * @ns:  대상 namespace (읽기 전용)
 * @req: 원본 NVMe 요청 (cdw10=NUMD(전송 dword 수), cdw11.eds=Extended Data Structure 비트)
 *
 * NVMe spec §8.19.6.4: Reservation Report는 현재 등록자 목록과 reservation 상태를
 * 호스트에게 반환하는 읽기 전용 커맨드이다. 상태 변경이 없으므로 직렬화 큐를 통하지 않고
 * 즉시 실행된다.
 *
 * NVMe-oF는 128비트 Host ID를 사용하므로 반드시 Extended Data Structure(EDS=1)를
 * 사용해야 한다. EDS=0이면 HOSTID_INCONSISTENT_FORMAT 에러를 반환한다.
 *
 * 반환 데이터 구조 (spdk_nvme_reservation_status_extended_data):
 *   - 헤더: gen, rtype, ptpls, regctl
 *   - 등록자별: cntlid, rcsts(holder 여부), rkey, hostid(128비트)
 *
 * 실행 컨텍스트: subsystem->thread. nvmf_ns_reservation_request에서 직접 호출.
 *
 * 호출 체인:
 *   nvmf_ns_reservation_request → [nvmf_ns_reservation_report]
 *     → spdk_iov_xfer_from_buf (데이터 전송)
 */
static void
nvmf_ns_reservation_report(const struct spdk_nvmf_ns *ns,
			   struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	/* [한국어] NVMe 커맨드: cdw10=NUMD(Number of Dwords), cdw11.eds=Extended Data 비트 */
	struct spdk_nvmf_registrant *reg, *tmp;	/* [한국어] 등록자 순회용 포인터 (SAFE) */
	struct spdk_nvme_reservation_status_extended_data status_data = { 0 };
	/* [한국어] 반환 데이터: 헤더 + 등록자 배열 (Extended Controller Data Structure) */
	struct spdk_iov_xfer ix;		/* [한국어] iov 기반 출력 커서 */
	uint32_t transfer_len;			/* [한국어] 호스트가 요청한 전송 길이 (바이트) */
	uint32_t regctl = 0;			/* [한국어] 등록자 수 카운터 */
	uint8_t status = SPDK_NVME_SC_SUCCESS;	/* [한국어] 응답 status code */

	if (req->iovcnt == 0) {
		/* [한국어] 데이터 출력 버퍼 없음 — INVALID_FIELD */
		SPDK_ERRLOG("No data transfer specified for request. "
			    " Unable to transfer back response.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	if (!cmd->cdw11_bits.resv_report.eds) {
		/* [한국어] NVMe-oF는 128비트 Host ID를 사용하므로 EDS 비트 필수 */
		SPDK_ERRLOG("NVMeoF uses extended controller data structure, "
			    "please set EDS bit in cdw11 and try again\n");
		status = SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT;
		goto exit;
	}

	/* Number of Dwords of the Reservation Status data structure to transfer */
	transfer_len = (cmd->cdw10 + 1) * sizeof(uint32_t);
	/* [한국어] cdw10=NUMD: 0-based dword 수 → (NUMD+1)*4 바이트 */

	if (transfer_len < sizeof(struct spdk_nvme_reservation_status_extended_data)) {
		/* [한국어] 최소 헤더 크기보다 작으면 내부 에러 */
		status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto exit;
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	/* [한국어] 출력 iov 커서 초기화 — spdk_iov_xfer_from_buf로 순차 출력 */

	status_data.data.gen = ns->gen;			/* [한국어] NS generation counter */
	status_data.data.rtype = ns->rtype;		/* [한국어] 현재 Reservation Type */
	status_data.data.ptpls = ns->ptpl_activated;	/* [한국어] PTPL 활성화 여부 */

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		regctl++;	/* [한국어] 전체 등록자 수 카운트 */
	}

	/*
	 * We report the number of registrants as per the spec here, even if
	 * the iov isn't big enough to contain them all. In that case, the
	 * spdk_iov_xfer_from_buf() won't actually copy any of the remaining
	 * data; as it keeps track of the iov cursor itself, it's simplest to
	 * just walk the entire list anyway.
	 */
	status_data.data.regctl = regctl;
	/* [한국어] NVMe spec: regctl은 실제 등록자 수 (전송 여부와 무관하게 전체 수 보고) */

	spdk_iov_xfer_from_buf(&ix, &status_data, sizeof(status_data));
	/* [한국어] 헤더(status_data) 출력 — iov 커서 전진 */

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		/* [한국어] 각 등록자에 대해 Extended Controller Data 생성 */
		struct spdk_nvme_registered_ctrlr_extended_data ctrlr_data = { 0 };

		ctrlr_data.cntlid = reg->cntlid ? reg->cntlid : 0xffff;
		/* [한국어] cntlid=0이면 0xffff (reserved: ctrlr가 현재 연결되지 않음) */
		ctrlr_data.rcsts.status = (ns->holder == reg) ? true : false;
		/* [한국어] rcsts.status=1이면 이 등록자가 현재 reservation holder */
		ctrlr_data.rkey = reg->rkey;		/* [한국어] 이 등록자의 Reservation Key */
		spdk_uuid_copy((struct spdk_uuid *)ctrlr_data.hostid, &reg->hostid);
		/* [한국어] 128비트 Host ID 복사 */

		spdk_iov_xfer_from_buf(&ix, &ctrlr_data, sizeof(ctrlr_data));
		/* [한국어] 등록자 데이터 출력 — iov 커서 전진 (버퍼 초과 시 no-op) */
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	/* [한국어] Generic Command Status type */
	req->rsp->nvme_cpl.status.sc = status;	/* [한국어] status code 설정 */
	return;
}

/*
 * [한국어]
 * nvmf_ns_reservation_complete - reservation 커맨드 완료 콜백 (poll group 스레드)
 *
 * @ctx: spdk_nvmf_request 포인터 (완료할 요청)
 *
 * subsystem->thread에서 reservation 처리가 완료된 후,
 * 원래 요청을 수신한 poll group 스레드로 completion을 전달하기 위해
 * spdk_thread_send_msg로 이 함수가 호출된다.
 *
 * 실행 컨텍스트: 요청이 발생한 poll group의 스레드.
 *
 * 호출 체인:
 *   _nvmf_ns_reservation_update_done → spdk_thread_send_msg → [nvmf_ns_reservation_complete]
 *     → spdk_nvmf_request_complete (응답 전송)
 */
static void
nvmf_ns_reservation_complete(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;	/* [한국어] 완료할 NVMe 요청 */

	spdk_nvmf_request_complete(req);	/* [한국어] NVMe-oF 응답 전송 */
}

/*
 * [한국어]
 * ns_reservation_pg_io_wait_check - PREEMPT_ABORT 시 poll group별 io_waiting 카운트 확인
 *
 * @i: spdk_io_channel_iter (채널 순회 컨텍스트, ctx=ns)
 *
 * PREEMPT_ABORT 커맨드 처리 후, 각 poll group에서 preempt된 hostid에 대한
 * 미완료 IO(io_waiting)가 남아 있는지 확인한다.
 * io_waiting != 0이면 즉시 채널 순회를 종료하고 (채널 순회 status=io_waiting 반환),
 * io_waiting == 0이면 다음 채널로 계속 순회한다.
 *
 * 실행 컨텍스트: 각 poll group 스레드.
 *
 * 호출 체인:
 *   ns_reservation_next_io_wait_check → spdk_for_each_channel → [ns_reservation_pg_io_wait_check]
 *     → spdk_for_each_channel_continue
 */
static void
ns_reservation_pg_io_wait_check(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_ns *ns;			/* [한국어] 대상 namespace */
	struct spdk_nvmf_poll_group *group;		/* [한국어] 현재 poll group */
	struct spdk_nvmf_subsystem_poll_group *sgroup;	/* [한국어] subsystem별 poll group 정보 */
	struct spdk_nvmf_subsystem_pg_ns_info *pg_ns;	/* [한국어] poll group의 NS 캐시 */

	ns = spdk_io_channel_iter_get_ctx(i);		/* [한국어] 순회 컨텍스트에서 ns 추출 */
	group = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));
	/* [한국어] 채널에서 poll group 추출 */
	sgroup = &group->sgroups[ns->subsystem->id];	/* [한국어] subsystem 인덱스로 sgroup 접근 */
	pg_ns = &sgroup->ns_info[ns->nsid - 1];		/* [한국어] nsid-1 인덱스로 NS 캐시 접근 */

	/* Pass io_waiting count as result, this will provide the following:
	 *	1) If non-zero, this will immedately end the channel walk
	 *	2) If zero, this will continue to next pg to check their io_waiting.
	 *	3) If last pg reports 0, all IO waiting is done and completion is
	 *	called with 0
	 */
	spdk_for_each_channel_continue(i, pg_ns->preempt_abort.io_waiting);
	/* [한국어] io_waiting 카운트를 status로 전달:
	 *   0이면 다음 채널 순회 계속, 0이 아니면 채널 순회 중단 (done 콜백 호출) */
}

/* [한국어] 전방 선언: ns_reservation_pg_io_wait_check_done에서 필요 */
static void ns_reservation_sched_next_io_wait_check(struct spdk_nvmf_ns *ns);

/*
 * [한국어]
 * ns_reservation_pg_io_wait_check_done - io_wait_check 순회 완료 콜백
 *
 * @i:      spdk_io_channel_iter (순회 완료 컨텍스트, ctx=ns)
 * @status: 순회 결과 (0=모든 pg에서 io_waiting=0, 0이 아님=아직 대기 중인 IO 있음)
 *
 * status=0이면 모든 poll group에서 IO 대기가 완료됐으므로 update_done 호출.
 * status!=0이면 아직 대기 중 — 다음 polling 주기를 예약.
 *
 * 실행 컨텍스트: subsystem->thread.
 *
 * 호출 체인:
 *   spdk_for_each_channel(ns_reservation_pg_io_wait_check) → [ns_reservation_pg_io_wait_check_done]
 *     성공: → _nvmf_ns_reservation_update_done
 *     대기 중: → ns_reservation_sched_next_io_wait_check
 */
static void
ns_reservation_pg_io_wait_check_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_ns *ns = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 순회 컨텍스트에서 ns 추출 */

	if (!status) {
		/* [한국어] 모든 poll group에서 io_waiting=0 — IO 대기 완료 */
		SPDK_DEBUGLOG(nvmf, "subsystem: %p, nsid: %u done waiting on IOs\n",
			      ns->subsystem, ns->nsid);
		ns->preempt_abort->io_waiting_done = true;	/* [한국어] 완료 플래그 설정 */
		_nvmf_ns_reservation_update_done(ns->subsystem,
						 STAILQ_FIRST(&ns->reservations), 0);
		/* [한국어] 정상 완료로 update_done 호출 → 요청 응답 전송 */
	} else {
		/* [한국어] 아직 IO 대기 중 — 다음 polling 주기 예약 */
		SPDK_DEBUGLOG(nvmf, "subsystem: %p, nsid: %u still waiting on %i IOs\n",
			      ns->subsystem, ns->nsid, status);
		ns_reservation_sched_next_io_wait_check(ns);
	}
}

/*
 * [한국어]
 * ns_reservation_pg_io_wait_clear_done - io_wait_clear 순회 완료 콜백 (타임아웃 경로)
 *
 * @i:      spdk_io_channel_iter
 * @status: 사용하지 않음 (항상 타임아웃으로 처리)
 *
 * io_waiting_timeout_ticks를 초과한 경우 이 콜백이 호출된다.
 * -ETIMEDOUT으로 update_done을 호출해 요청을 COMMAND_INTERRUPTED로 완료시킨다.
 *
 * 호출 체인:
 *   spdk_for_each_channel(ns_reservation_pg_io_wait_clear) → [ns_reservation_pg_io_wait_clear_done]
 *     → _nvmf_ns_reservation_update_done(-ETIMEDOUT)
 */
static void
ns_reservation_pg_io_wait_clear_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_ns *ns = (struct spdk_nvmf_ns *)spdk_io_channel_iter_get_ctx(i);
	/* If we entered this function we are always timed out */
	/* [한국어] 타임아웃 경로 — 항상 -ETIMEDOUT으로 완료 */
	_nvmf_ns_reservation_update_done(ns->subsystem,
					 STAILQ_FIRST(&ns->reservations), -ETIMEDOUT);
}

/*
 * [한국어]
 * ns_reservation_pg_io_wait_clear - 타임아웃 시 poll group별 reservation_waiting 플래그 강제 해제
 *
 * @i: spdk_io_channel_iter (채널 순회 컨텍스트, ctx=ns)
 *
 * NS_RESERVATION_IO_WAIT_TIMEOUT_S 초과 시 이 함수가 각 poll group에서 호출된다.
 * preempt_abort->hostids에 있는 hostid의 qpair에서 이 NS의 outstanding 요청 중
 * reservation_waiting=1인 것을 모두 0으로 해제한다.
 * 이후 채널 순회를 계속 (status=0).
 *
 * 실행 컨텍스트: 각 poll group 스레드.
 *
 * 호출 체인:
 *   ns_reservation_next_io_wait_check → spdk_for_each_channel → [ns_reservation_pg_io_wait_clear]
 *     → spdk_for_each_channel_continue(0)
 */
static void
ns_reservation_pg_io_wait_clear(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_ns *ns = (struct spdk_nvmf_ns *)spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 순회 컨텍스트에서 ns 추출 */
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));
	/* [한국어] 채널에서 poll group 추출 */
	struct spdk_nvmf_request *q_req;		/* [한국어] outstanding 요청 순회용 */
	struct spdk_nvmf_qpair *qpair;			/* [한국어] qpair 순회용 */
	struct spdk_nvmf_reservation_preempt_abort_info *p_info = ns->preempt_abort;
	/* [한국어] PREEMPT_ABORT 정보 (hostids 목록) */
	bool hostid_match;				/* [한국어] hostid 일치 여부 */

	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		/* [한국어] 이 poll group의 모든 qpair 순회 */
		if (!qpair->ctrlr || qpair->ctrlr->subsys != ns->subsystem) {
			/* [한국어] ctrlr 없거나 다른 subsystem의 qpair — 스킵 */
			continue;
		}
		hostid_match = ns_reservation_hostid_list_contains_id(p_info->hostids,
				p_info->hostids_cnt, &qpair->ctrlr->hostid);
		/* [한국어] 이 qpair의 ctrlr가 preempt된 hostid인지 확인 */
		if (!hostid_match) {
			continue;	/* [한국어] preempt 대상 아님 — 스킵 */
		}
		TAILQ_FOREACH(q_req, &qpair->outstanding, link) {
			/* [한국어] 이 qpair의 outstanding 요청 순회 */
			struct spdk_nvme_cmd *req_cmd = &q_req->cmd->nvme_cmd;
			if (req_cmd->nsid == ns->nsid && q_req->reservation_waiting) {
				/* [한국어] 이 NS의 reservation_waiting 요청 — 강제 해제 */
				q_req->reservation_waiting = 0;
			}
		}
	}
	spdk_for_each_channel_continue(i, 0);	/* [한국어] 다음 채널 순회 계속 */
}

/*
 * [한국어]
 * ns_reservation_next_io_wait_check - poller 콜백: 다음 io_waiting 점검 실행
 *
 * @ctx: struct spdk_nvmf_ns 포인터
 * @return: SPDK_POLLER_BUSY (항상)
 *
 * ns_reservation_sched_next_io_wait_check에 의해 등록된 one-shot poller.
 * 타임아웃 이전이면 poll group별 io_waiting 점검(ns_reservation_pg_io_wait_check)을 시작하고,
 * 타임아웃 이후이면 강제 해제(ns_reservation_pg_io_wait_clear)를 시작한다.
 * 자기 자신을 해제(spdk_poller_unregister)하여 one-shot 동작을 구현.
 *
 * 실행 컨텍스트: subsystem->thread.
 *
 * 호출 체인:
 *   SPDK_POLLER_REGISTER → [ns_reservation_next_io_wait_check]
 *     타임아웃 전: → spdk_for_each_channel(ns_reservation_pg_io_wait_check)
 *     타임아웃 후: → spdk_for_each_channel(ns_reservation_pg_io_wait_clear)
 */
static int
ns_reservation_next_io_wait_check(void *ctx)
{
	struct spdk_nvmf_ns *ns = (struct spdk_nvmf_ns *)ctx;	/* [한국어] 대상 namespace */
	struct spdk_nvmf_reservation_preempt_abort_info *p_info = ns->preempt_abort;
	/* [한국어] PREEMPT_ABORT 정보 구조체 */

	/* this should not be running if io_waiting is complete */
	assert(!p_info->io_waiting_done);	/* [한국어] 이미 완료됐으면 이 poller는 실행되지 않아야 함 */

	if (spdk_get_ticks() < p_info->io_waiting_timeout_ticks) {
		/* Start a poll group check */
		/* [한국어] 타임아웃 이전 — 모든 poll group에서 io_waiting 점검 */
		spdk_for_each_channel(ns->subsystem->tgt,
				      ns_reservation_pg_io_wait_check,
				      ns,
				      ns_reservation_pg_io_wait_check_done);
	} else {
		/* If the cmd timed out we call update_done during cleanup */
		/* [한국어] 타임아웃 이후 — reservation_waiting 강제 해제 후 타임아웃 완료 */
		spdk_for_each_channel(ns->subsystem->tgt,
				      ns_reservation_pg_io_wait_clear,
				      ns,
				      ns_reservation_pg_io_wait_clear_done);
	}

	spdk_poller_unregister(&p_info->io_waiting_timer);
	/* [한국어] one-shot poller 자기 해제 — 다음 체크는 sched_next에서 재등록 */
	return SPDK_POLLER_BUSY;
}

/* [한국어] PREEMPT_ABORT io_waiting 점검 주기 (마이크로초): 100μs */
#define NS_RESERVATION_IO_WAIT_CHECK_INTERVAL 100
/* [한국어] PREEMPT_ABORT io_waiting 타임아웃 (초): 10초 초과 시 강제 완료 */
#define NS_RESERVATION_IO_WAIT_TIMEOUT_S 10
/*
 * [한국어]
 * ns_reservation_sched_next_io_wait_check - 다음 io_waiting 점검 poller 예약
 *
 * @ns: 대상 namespace
 *
 * io_waiting_timeout_ticks를 처음 계산하고 (최초 호출 시),
 * NS_RESERVATION_IO_WAIT_CHECK_INTERVAL μs 후에 ns_reservation_next_io_wait_check를
 * one-shot poller로 등록한다.
 *
 * 실행 컨텍스트: subsystem->thread.
 *
 * 호출 체인:
 *   ns_reservation_pg_io_wait_check_done(status!=0) → [ns_reservation_sched_next_io_wait_check]
 *     → SPDK_POLLER_REGISTER(ns_reservation_next_io_wait_check)
 */
static void
ns_reservation_sched_next_io_wait_check(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_reservation_preempt_abort_info *p_info = ns->preempt_abort;
	/* [한국어] PREEMPT_ABORT 정보 구조체 */
	assert(p_info);				/* [한국어] preempt_abort가 초기화되어 있어야 함 */
	assert(p_info->io_waiting_timer == NULL);/* [한국어] 이미 실행 중인 타이머 없어야 함 */

	/* First time scheduling, calculate a total timeout */
	if (!p_info->io_waiting_timeout_ticks) {
		/* [한국어] 최초 호출 시 절대 타임아웃 ticks 계산 */
		p_info->io_waiting_timeout_ticks = spdk_get_ticks() +
						   NS_RESERVATION_IO_WAIT_TIMEOUT_S * spdk_get_ticks_hz();
		/* [한국어] 현재 ticks + 10초*ticks_per_sec = 10초 후 절대 시각 */
	}
	/* We use a poller as a one-shot timer for next check */
	p_info->io_waiting_timer =
		SPDK_POLLER_REGISTER(ns_reservation_next_io_wait_check, ns, NS_RESERVATION_IO_WAIT_CHECK_INTERVAL);
	/* [한국어] 100μs 후 one-shot poller 등록 — 완료되면 자기 해제 */
}

/*
 * [한국어]
 * _nvmf_ns_reservation_update_done - reservation 상태 전파 완료 콜백 (subsystem->thread)
 *
 * @subsystem: 대상 subsystem
 * @cb_arg:    완료된 spdk_nvmf_request 포인터
 * @status:    전파 결과 (0=성공, -EINVAL/-ENOMEM/-ETIMEDOUT=에러)
 *
 * spdk_for_each_channel(ns_reservation_pg_update) 완료 후 이 함수가 호출된다.
 * 에러 발생 시 요청 응답의 status code를 적절히 설정한다.
 *
 * PREEMPT_ABORT 처리 특수 케이스:
 *   성공(status=0)이고 io_waiting이 아직 완료되지 않은 경우, io_waiting 점검을 시작한다.
 *   io_waiting 완료 후 이 함수가 다시 호출돼 정상 완료 경로로 진행한다.
 *
 * 완료 후:
 *   - 이 요청을 ns->reservations 큐에서 제거
 *   - 큐에 다음 요청이 있으면 subsystem->thread에서 처리 시작
 *   - poll group 스레드로 nvmf_ns_reservation_complete 전달
 *
 * 실행 컨텍스트: subsystem->thread (assert 검증).
 *
 * 호출 체인:
 *   ns_reservation_pg_update_done → [_nvmf_ns_reservation_update_done]
 *   ns_reservation_pg_io_wait_check_done → [_nvmf_ns_reservation_update_done]
 *     → spdk_thread_send_msg → nvmf_ns_reservation_complete
 */
static void
_nvmf_ns_reservation_update_done(struct spdk_nvmf_subsystem *subsystem,
				 void *cb_arg, int status)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)cb_arg;
	/* [한국어] 완료할 reservation 요청 */
	struct spdk_nvmf_poll_group *group = req->qpair->group;
	/* [한국어] 요청이 발생한 poll group (completion 전달 대상) */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;	/* [한국어] nsid 추출용 커맨드 */
	struct spdk_nvmf_ns *ns;				/* [한국어] 대상 namespace */

	assert(subsystem->thread == spdk_get_thread());
	/* [한국어] 이 함수는 반드시 subsystem->thread에서 호출되어야 함 */

	if (status != 0) {
		/* [한국어] 에러 발생 시 요청 응답에 적절한 status code 설정 */
		switch (status) {
		case -EINVAL:
			SPDK_ERRLOG("ns_reservation failed invalid field\n");
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
			break;
		case -ENOMEM:
			SPDK_ERRLOG("ns_reservation failed internal device error\n");
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			break;
		case -ETIMEDOUT:
			/* [한국어] PREEMPT_ABORT IO 대기 타임아웃 */
			SPDK_ERRLOG("ns_reservation failed due to time out: %i\n", status);
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_COMMAND_INTERRUPTED;
			break;
		default:
			SPDK_ERRLOG("ns_reservation failed unknown error: %i\n", status);
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_UNRECOVERED_ERROR;
			break;
		}

	}
	/* Get namespace */
	ns = _nvmf_subsystem_get_ns(subsystem, cmd->nsid);
	/* [한국어] 커맨드의 nsid로 NS 포인터 획득 */
	assert(ns != NULL);	/* [한국어] NS가 반드시 존재해야 함 */

	/* sanity check: this req should be head of outstanding */
	assert(req->reservation_queued == true);
	/* [한국어] 이 요청은 reservation 큐에 들어있어야 함 */
	assert(req == STAILQ_FIRST(&ns->reservations));
	/* [한국어] 이 요청이 큐의 head여야 함 (직렬화 보장) */

	if (!status && ns_reservation_req_is_preempt_abort(req) && !ns->preempt_abort->io_waiting_done) {
		/* Check for io_waiting completion */
		/* [한국어] PREEMPT_ABORT 성공이지만 IO 대기 미완료 — io_waiting 점검 시작 */
		spdk_for_each_channel(ns->subsystem->tgt,
				      ns_reservation_pg_io_wait_check,
				      ns,
				      ns_reservation_pg_io_wait_check_done);
		return;	/* [한국어] io_waiting 완료 후 이 함수가 다시 호출됨 */
	}

	/* req is complete, remove from queue and continue if there's others */
	STAILQ_REMOVE_HEAD(&ns->reservations, reservation_link);
	/* [한국어] 완료된 요청을 serialization 큐에서 제거 */
	req->reservation_queued = false;	/* [한국어] 큐에서 제거됨을 표시 */
	if (!STAILQ_EMPTY(&ns->reservations)) {
		/* NOTE: we leave the next on the queue to prevent any in-flight
		 * requests moving from pg->thread to subsystem->thread from
		 * executing before the next one
		 */
		/* [한국어] 다음 요청이 있으면 subsystem->thread에서 처리 시작
		 * 큐에 남겨두는 이유: pg->thread에서 subsystem->thread로 이동 중인
		 * 요청이 먼저 실행되지 않도록 serialization 보장 */
		spdk_thread_send_msg(subsystem->thread, nvmf_ns_reservation_request,
				     STAILQ_FIRST(&ns->reservations));
	}

	/* Complete the request on the original pg */
	spdk_thread_send_msg(group->thread, nvmf_ns_reservation_complete, req);
	/* [한국어] 원래 poll group 스레드로 completion 전달 — 응답 전송 */
}

/*
 * [한국어]
 * nvmf_ns_reservation_update_state - reservation 상태 변경 커맨드 직렬화 및 실행
 *
 * @ns:   대상 namespace
 * @ctrlr: 커맨드를 발송한 ctrlr
 * @req:  원본 NVMe 요청
 * @opc:  NVMe opcode (REGISTER/ACQUIRE/RELEASE)
 *
 * 모든 reservation 상태 변경 커맨드는 ns->reservations STAILQ를 통해 직렬화된다.
 * 이미 진행 중인 커맨드가 있으면 큐에만 삽입하고 반환 (나중에 재호출됨).
 * 큐 head이면 즉시 해당 커맨드를 실행한다.
 *
 * 상태 변경이 발생하면 (update_sgroup=true):
 *   1. PTPL 활성화 또는 REGISTER 커맨드면 JSON 파일 업데이트
 *   2. spdk_for_each_channel로 모든 poll group 캐시 업데이트
 *   완료 후 _nvmf_ns_reservation_update_done 호출.
 *
 * 상태 변경 없으면 즉시 _nvmf_ns_reservation_update_done 호출.
 *
 * 실행 컨텍스트: subsystem->thread.
 *
 * 호출 체인:
 *   nvmf_ns_reservation_request → [nvmf_ns_reservation_update_state]
 *     → nvmf_ns_reservation_register/acquire/release
 *     → spdk_for_each_channel(ns_reservation_pg_update) → ns_reservation_pg_update_done
 *     → _nvmf_ns_reservation_update_done
 */
static void
nvmf_ns_reservation_update_state(struct spdk_nvmf_ns *ns,
				 struct spdk_nvmf_ctrlr *ctrlr,
				 struct spdk_nvmf_request *req,
				 enum spdk_nvme_nvm_opcode opc)
{
	bool update_sgroup = false;	/* [한국어] poll group 캐시 갱신 필요 여부 */
	int status = 0;			/* [한국어] 즉시 완료 시 전달할 status */

	/* All reservation state modifications must be queued to serialize them */
	if (!req->reservation_queued) {
		/* [한국어] 아직 큐에 없으면 STAILQ tail에 삽입 */
		STAILQ_INSERT_TAIL(&ns->reservations, req, reservation_link);
		req->reservation_queued = true;	/* [한국어] 큐에 삽입됨 표시 */
	}
	/* The head is in-progress, others must wait */
	if (req != STAILQ_FIRST(&ns->reservations)) {
		/* [한국어] 큐 head가 아님 — 선행 요청 완료 후 재호출됨 */
		return;
	}

	switch (opc) {
	case SPDK_NVME_OPC_RESERVATION_REGISTER:
		update_sgroup = nvmf_ns_reservation_register(ns, ctrlr, req);
		/* [한국어] REGISTER 처리: 등록/해제/교체 */
		break;
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		update_sgroup = nvmf_ns_reservation_acquire(ns, ctrlr, req);
		/* [한국어] ACQUIRE 처리: 획득/preempt */
		break;
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		update_sgroup = nvmf_ns_reservation_release(ns, ctrlr, req);
		/* [한국어] RELEASE 처리: 해제/clear */
		break;
	default:
		break;
	}

	/* update reservation information to subsystem's poll group */
	if (update_sgroup) {
		/* [한국어] 상태 변경 발생 — poll group 캐시 업데이트 필요 */
		if (ns->ptpl_activated || opc == SPDK_NVME_OPC_RESERVATION_REGISTER) {
			/* [한국어] PTPL 활성화 또는 REGISTER 커맨드면 JSON 파일 업데이트 */
			if (nvmf_ns_update_reservation_info(ns) != 0) {
				req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				/* [한국어] JSON 업데이트 실패 시 에러 응답 (그래도 pg 캐시는 갱신) */
			}
		}
		spdk_for_each_channel(ns->subsystem->tgt,
				      ns_reservation_pg_update,
				      ns,
				      ns_reservation_pg_update_done);
		/* [한국어] 모든 poll group에 변경된 PR 상태 전파 — 비동기 완료 */
		return;	/* [한국어] 완료는 ns_reservation_pg_update_done → _nvmf_ns_reservation_update_done */
	}

	_nvmf_ns_reservation_update_done(ctrlr->subsys, req, status);
	/* [한국어] 상태 변경 없으면 즉시 완료 처리 */
}

/*
 * [한국어]
 * nvmf_ns_reservation_request - reservation 커맨드 처리 진입점 (subsystem->thread)
 *
 * @ctx: spdk_nvmf_request 포인터
 *
 * poll group 스레드에서 spdk_thread_send_msg로 subsystem->thread로 전달된 후
 * 이 함수가 실행된다.
 *
 * RESERVATION_REPORT는 읽기 전용이므로 직렬화 없이 즉시 실행하고,
 * REGISTER/ACQUIRE/RELEASE는 상태 변경이므로 nvmf_ns_reservation_update_state로 직렬화.
 *
 * 실행 컨텍스트: subsystem->thread.
 *
 * 호출 체인:
 *   nvmf_ctrlr_process_admin_cmd/io_cmd(poll group) → spdk_thread_send_msg
 *     → [nvmf_ns_reservation_request]
 *       REPORT: → nvmf_ns_reservation_report → nvmf_ns_reservation_complete
 *       기타:   → nvmf_ns_reservation_update_state → ...비동기...
 */
void
nvmf_ns_reservation_request(void *ctx)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)ctx;
	/* [한국어] poll group 스레드에서 전달된 reservation 요청 */
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;	/* [한국어] opcode와 nsid 추출용 */
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;	/* [한국어] 발송 ctrlr */
	uint32_t nsid;						/* [한국어] 대상 NS ID */
	struct spdk_nvmf_ns *ns;				/* [한국어] 대상 namespace 포인터 */

	nsid = cmd->nsid;					/* [한국어] NVMe 커맨드에서 NSID 추출 */
	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, nsid);	/* [한국어] NSID로 NS 포인터 획득 */
	assert(ns != NULL);	/* [한국어] 유효한 NS여야 함 */

	/* Report is a read-only command and can always be executed */
	if (cmd->opc == SPDK_NVME_OPC_RESERVATION_REPORT) {
		/* [한국어] REPORT: 상태 읽기 전용 — 직렬화 불필요 */
		nvmf_ns_reservation_report(ns, req);
		/* Complete the request on the original pg */
		spdk_thread_send_msg(req->qpair->group->thread, nvmf_ns_reservation_complete, req);
		/* [한국어] poll group 스레드로 completion 전달 */
	} else {
		/* Remaining commands modify reservation state and must be serialized.
		 * These complete asynchronously after state propagates to poll groups
		 */
		/* [한국어] 상태 변경 커맨드 — 직렬화 큐를 통해 처리 (비동기 완료) */
		nvmf_ns_reservation_update_state(ns, ctrlr, req, cmd->opc);
	}
}

/*
 * [한국어]
 * nvmf_ns_is_ptpl_capable_json - JSON 백엔드 기반 PTPL 지원 여부 확인
 *
 * @ns: 대상 namespace
 * @return: ns->ptpl_file이 설정되어 있으면 true
 *
 * 기본(JSON) 구현: ptpl_file 경로가 설정된 NS는 PTPL 가능.
 * spdk_nvmf_set_custom_ns_reservation_ops로 교체 가능.
 *
 * 실행 컨텍스트: subsystem->thread.
 */
static bool
nvmf_ns_is_ptpl_capable_json(const struct spdk_nvmf_ns *ns)
{
	return ns->ptpl_file != NULL;
	/* [한국어] ptpl_file이 설정돼 있으면 JSON 파일 기반 PTPL 지원 */
}

/* [한국어] 기본 reservation 작업 함수 테이블 (JSON 백엔드 구현)
 * spdk_nvmf_set_custom_ns_reservation_ops()로 커스텀 구현으로 교체 가능.
 * 전역 단일 인스턴스 — 프로세스 전체에 적용됨. */
static struct spdk_nvmf_ns_reservation_ops g_reservation_ops = {
	.is_ptpl_capable = nvmf_ns_is_ptpl_capable_json,	/* [한국어] PTPL 지원 여부 판단 함수 */
	.update = nvmf_ns_reservation_update_json,		/* [한국어] PR 상태 → JSON 파일 저장 */
	.load = nvmf_ns_reservation_load_json,			/* [한국어] JSON 파일 → PR 상태 복원 */
};

/*
 * [한국어]
 * nvmf_ns_is_ptpl_capable - NS의 PTPL 지원 여부 확인 (공개 API)
 *
 * @ns: 대상 namespace
 * @return: PTPL 지원이면 true
 *
 * g_reservation_ops.is_ptpl_capable를 통해 실제 구현을 호출한다.
 * 커스텀 ops가 설정된 경우 해당 구현이 사용된다.
 *
 * 호출자: nvmf_ns_reservation_register (CPTPL 처리), nvmf_ns_get_rescap (RESCAP.ptpls)
 */
bool
nvmf_ns_is_ptpl_capable(const struct spdk_nvmf_ns *ns)
{
	return g_reservation_ops.is_ptpl_capable(ns);
	/* [한국어] ops 테이블의 is_ptpl_capable 함수 포인터를 통해 호출 */
}

/*
 * [한국어]
 * nvmf_ns_get_rescap - NS의 Reservation Capabilities 반환
 *
 * @ns: 대상 namespace
 * @return: spdk_nvme_rescap 구조체 (IDENTIFY NS 응답의 RESCAP 필드)
 *
 * NVMe spec §5.15.1: Reservation Capabilities(RESCAP) 필드는 IDENTIFY NS 응답에서
 * 호스트에게 이 NS가 지원하는 reservation 기능을 알린다.
 *
 * SPDK NVMe-oF는 모든 6가지 reservation type을 지원하며 IEKEY도 지원.
 * PTPL 지원 여부는 NS별로 다름 (ptpl_file 설정 여부).
 *
 * 호출자: nvmf_ns_identify (IDENTIFY NS 응답 구성 시)
 */
struct spdk_nvme_rescap
nvmf_ns_get_rescap(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvme_rescap rescap = {
		.ptpls = nvmf_ns_is_ptpl_capable(ns),	/* [한국어] PTPL 지원 여부 */
		.wes = 1,	/* [한국어] Write Exclusive 지원 */
		.eas = 1,	/* [한국어] Exclusive Access 지원 */
		.weros = 1,	/* [한국어] Write Exclusive - Registrants Only 지원 */
		.earos = 1,	/* [한국어] Exclusive Access - Registrants Only 지원 */
		.wears = 1,	/* [한국어] Write Exclusive - All Registrants 지원 */
		.eaars = 1,	/* [한국어] Exclusive Access - All Registrants 지원 */
		.ieks = 1,	/* [한국어] Ignore Existing Key 지원 */
	};
	return rescap;
}

/*
 * [한국어]
 * nvmf_ns_reservation_update - PR 상태를 영구 저장소에 업데이트 (래퍼)
 *
 * @ns:   대상 namespace
 * @info: 저장할 reservation 정보
 * @return: 0=성공, 음수=에러
 *
 * g_reservation_ops.update를 호출하는 thin wrapper.
 * 기본 구현은 nvmf_ns_reservation_update_json (JSON 파일 저장).
 */
static int
nvmf_ns_reservation_update(const struct spdk_nvmf_ns *ns,
			   const struct spdk_nvmf_reservation_info *info)
{
	return g_reservation_ops.update(ns, info);
	/* [한국어] ops 테이블의 update 함수 포인터를 통해 호출 */
}

/*
 * [한국어]
 * nvmf_ns_reservation_load - 영구 저장소에서 PR 상태 로드 (래퍼)
 *
 * @ns:   대상 namespace
 * @info: 로드된 reservation 정보를 저장할 구조체
 * @return: 0=성공, 음수=에러
 *
 * g_reservation_ops.load를 호출하는 thin wrapper.
 * 기본 구현은 nvmf_ns_reservation_load_json (JSON 파일 로드).
 */
static int
nvmf_ns_reservation_load(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info)
{
	return g_reservation_ops.load(ns, info);
	/* [한국어] ops 테이블의 load 함수 포인터를 통해 호출 */
}

/*
 * [한국어]
 * spdk_nvmf_set_custom_ns_reservation_ops - 커스텀 reservation 작업 함수 테이블 설정
 *
 * @ops: 교체할 reservation 작업 함수 테이블 포인터
 *
 * 기본 JSON 기반 PTPL 구현 대신 외부에서 제공하는 커스텀 구현으로 교체.
 * 예: 데이터베이스 기반 PTPL, 메모리 기반 PTPL 등.
 * 이 함수 호출 이후 모든 PTPL 관련 동작은 커스텀 ops를 통해 수행된다.
 *
 * 주의: 프로세스 전체에 전역적으로 적용됨 — 스레드 안전하지 않으므로
 * subsystem 시작 전에 호출해야 한다.
 */
void
spdk_nvmf_set_custom_ns_reservation_ops(const struct spdk_nvmf_ns_reservation_ops *ops)
{
	g_reservation_ops = *ops;	/* [한국어] 전역 ops 테이블을 커스텀 구현으로 교체 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_set_ana_reporting - subsystem의 ANA reporting 활성화/비활성화
 *
 * @subsystem:     대상 subsystem
 * @ana_reporting: true=ANA reporting 활성화, false=비활성화
 * @return: 0=성공, -EAGAIN=subsystem이 INACTIVE 상태가 아님
 *
 * ANA(Asymmetric Namespace Access) reporting은 NVMe-oF 멀티패스 환경에서
 * 호스트가 각 path의 접근 상태를 알 수 있도록 하는 기능이다.
 * 활성화하면 IDENTIFY CONTROLLER 응답에서 CMIC.ana bit가 설정된다.
 * subsystem이 INACTIVE 상태일 때만 변경 가능.
 *
 * 호출자: RPC nvmf_subsystem_add_listener 등 설정 시점.
 */
int
spdk_nvmf_subsystem_set_ana_reporting(struct spdk_nvmf_subsystem *subsystem,
				      bool ana_reporting)
{
	if (subsystem->state != SPDK_NVMF_SUBSYSTEM_INACTIVE) {
		/* [한국어] INACTIVE가 아닌 상태에서는 ANA reporting 변경 불가 */
		return -EAGAIN;
	}

	subsystem->flags.ana_reporting = ana_reporting;
	/* [한국어] flags.ana_reporting 설정 — IDENTIFY CONTROLLER 응답에 반영됨 */

	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_ana_reporting - subsystem의 ANA reporting 활성화 여부 반환
 *
 * @subsystem: 대상 subsystem
 * @return: true=ANA reporting 활성화됨
 *
 * 호출자: IDENTIFY CONTROLLER 응답 구성, RPC 상태 조회.
 */
bool
spdk_nvmf_subsystem_get_ana_reporting(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->flags.ana_reporting;	/* [한국어] ANA reporting 플래그 반환 */
}

struct subsystem_listener_update_ctx {
	struct spdk_nvmf_subsystem_listener *listener;
	/* [한국어] ANA 상태가 변경된 listener.
	 * 설정자: spdk_nvmf_subsystem_set_ana_state에서 calloc + listener 설정.
	 * 읽는 자: subsystem_listener_update_on_pg에서 AEN 발송 대상 확인.
	 * 값 범위: 유효한 listener 포인터 (NULL 불가).
	 * 동기화: spdk_for_each_channel 순회 중에만 사용 — 단일 순회 컨텍스트. */

	spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn;
	/* [한국어] 모든 poll group 통지 완료 후 호출할 완료 콜백.
	 * 설정자: spdk_nvmf_subsystem_set_ana_state에서 cb_fn 인자로 설정.
	 * 읽는 자: subsystem_listener_update_done에서 호출.
	 * 값 범위: NULL이면 완료 통지 없음 (assert(cb_fn!=NULL)로 NULL 차단됨).
	 * 동기화: 채널 순회 완료 후 단일 스레드에서 호출 — 락 불필요. */

	void *cb_arg;
	/* [한국어] cb_fn에 전달할 사용자 정의 컨텍스트.
	 * 설정자: spdk_nvmf_subsystem_set_ana_state에서 cb_arg 인자로 설정.
	 * 읽는 자: subsystem_listener_update_done에서 cb_fn(cb_arg, status) 호출.
	 * 값 범위: 임의의 포인터 (NULL 가능).
	 * 동기화: 채널 순회 완료 후 단일 스레드에서 사용 — 락 불필요. */
};

/*
 * [한국어]
 * subsystem_listener_update_done - 모든 poll group ANA 통지 완료 콜백
 *
 * @i:      spdk_io_channel_iter (순회 완료 컨텍스트)
 * @status: 순회 결과 (항상 0 — 에러 없음)
 *
 * spdk_for_each_channel(subsystem_listener_update_on_pg) 완료 후 호출.
 * 사용자 완료 콜백을 호출하고 ctx 메모리를 해제한다.
 *
 * 실행 컨텍스트: subsystem->thread.
 *
 * 호출 체인:
 *   spdk_for_each_channel(subsystem_listener_update_on_pg) → [subsystem_listener_update_done]
 *     → ctx->cb_fn (사용자 콜백)
 */
static void
subsystem_listener_update_done(struct spdk_io_channel_iter *i, int status)
{
	struct subsystem_listener_update_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 순회 컨텍스트에서 ctx 추출 */

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, status);	/* [한국어] 완료 콜백 호출 */
	}
	free(ctx);	/* [한국어] ctx 메모리 해제 */
}

/*
 * [한국어]
 * subsystem_listener_update_on_pg - 각 poll group에서 ANA 변경 AER 발송
 *
 * @i: spdk_io_channel_iter (채널 순회 컨텍스트, ctx=subsystem_listener_update_ctx)
 *
 * listener와 연결된 ctrlr의 admin qpair가 이 poll group에 속하면
 * ANA Change AER(Async Event Request)을 발송한다.
 *
 * 실행 컨텍스트: 각 poll group 스레드.
 *
 * 호출 체인:
 *   spdk_for_each_channel → [subsystem_listener_update_on_pg]
 *     → nvmf_ctrlr_async_event_ana_change_notice
 *     → spdk_for_each_channel_continue
 */
static void
subsystem_listener_update_on_pg(struct spdk_io_channel_iter *i)
{
	struct subsystem_listener_update_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	/* [한국어] 순회 컨텍스트에서 ctx 추출 */
	struct spdk_nvmf_subsystem_listener *listener;	/* [한국어] ANA 상태 변경된 listener */
	struct spdk_nvmf_poll_group *group;		/* [한국어] 현재 poll group */
	struct spdk_nvmf_ctrlr *ctrlr;			/* [한국어] subsystem의 ctrlr 순회용 */

	listener = ctx->listener;			/* [한국어] 변경된 listener */
	group = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));
	/* [한국어] 현재 채널의 poll group */

	TAILQ_FOREACH(ctrlr, &listener->subsystem->ctrlrs, link) {
		/* [한국어] subsystem의 모든 ctrlr 순회 */
		if (ctrlr->thread != spdk_get_thread()) {
			/* [한국어] 이 poll group 스레드의 ctrlr가 아님 — 스킵 */
			continue;
		}

		if (ctrlr->admin_qpair && ctrlr->admin_qpair->group == group && ctrlr->listener == listener) {
			/* [한국어] 이 poll group에 속한 admin qpair이고 이 listener와 연결됨
			 * — ANA 상태 변경 AER 발송 */
			nvmf_ctrlr_async_event_ana_change_notice(ctrlr);
		}
	}

	spdk_for_each_channel_continue(i, 0);	/* [한국어] 다음 채널 순회 계속 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_set_ana_state - 특정 listener의 ANA 그룹 상태 변경 (멀티패스)
 *
 * @subsystem: 대상 subsystem
 * @trid:      대상 listener의 transport id
 * @ana_state: OPTIMIZED / NON_OPTIMIZED / INACCESSIBLE / PERSISTENT_LOSS / CHANGE
 * @anagrpid:  변경할 ANA 그룹 id (0=모든 그룹)
 * @cb_fn/cb_arg: 모든 poll group 통지가 끝나면 호출될 완료 콜백
 *
 * ANA(Asymmetric Namespace Access)는 NVMe-oF 스펙 1.1+에서 도입된 멀티패스
 * 메커니즘이다. 호스트 측 multipath 드라이버는 각 path(=listener)의 ANA
 * 상태를 보고 어느 path로 IO를 보낼지 결정한다 (예: OPTIMIZED 우선).
 * 본 함수는 listener의 ana_state 배열을 업데이트하고, 모든 poll group의
 * ctrlr에게 ANA Change AER(Async Event Request)을 발생시킨다.
 *
 * 실행 컨텍스트: subsystem->thread. spdk_for_each_channel로 모든 poll group
 * 순회하며 비동기로 통지.
 *
 * 호출 체인:
 *   RPC subsystem_listener_set_ana_state → [spdk_nvmf_subsystem_set_ana_state]
 *     → spdk_for_each_channel → subsystem_listener_update_on_pg
 *     → nvmf_ctrlr_async_event_ana_change_notice
 */
void
spdk_nvmf_subsystem_set_ana_state(struct spdk_nvmf_subsystem *subsystem,
				  const struct spdk_nvme_transport_id *trid,
				  enum spdk_nvme_ana_state ana_state, uint32_t anagrpid,
				  spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn, void *cb_arg)
{
	struct spdk_nvmf_subsystem_listener *listener;
	struct subsystem_listener_update_ctx *ctx;
	uint32_t i;

	assert(cb_fn != NULL);
	/* [한국어] 완료 콜백은 반드시 제공되어야 함 */
	assert(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	       subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED);
	/* [한국어] ANA 상태 변경은 INACTIVE 또는 PAUSED 상태에서만 가능 */

	if (!subsystem->flags.ana_reporting) {
		/* [한국어] ANA reporting이 비활성화된 subsystem에서는 ANA 상태 변경 불가 */
		SPDK_ERRLOG("ANA reporting is disabled\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	/* ANA Change state is not used, ANA Persistent Loss state
	 * is not supported yet.
	 */
	/* [한국어] 지원되는 ANA 상태: OPTIMIZED / NON_OPTIMIZED / INACCESSIBLE
	 * CHANGE와 PERSISTENT_LOSS는 현재 미지원 */
	if (!(ana_state == SPDK_NVME_ANA_OPTIMIZED_STATE ||
	      ana_state == SPDK_NVME_ANA_NON_OPTIMIZED_STATE ||
	      ana_state == SPDK_NVME_ANA_INACCESSIBLE_STATE)) {
		SPDK_ERRLOG("ANA state %d is not supported\n", ana_state);
		cb_fn(cb_arg, -ENOTSUP);
		return;
	}

	if (anagrpid > subsystem->max_nsid) {
		/* [한국어] ANA group ID는 max_nsid를 초과할 수 없음 */
		SPDK_ERRLOG("ANA group ID %" PRIu32 " is more than maximum\n", anagrpid);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	listener = nvmf_subsystem_find_listener(subsystem, trid);
	/* [한국어] trid로 대상 listener 조회 */
	if (!listener) {
		SPDK_ERRLOG("Unable to find listener.\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (anagrpid != 0 && listener->ana_state[anagrpid - 1] == ana_state) {
		/* [한국어] 특정 그룹의 상태가 이미 요청 상태와 동일 — no-op */
		cb_fn(cb_arg, 0);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	/* [한국어] ANA 통지 컨텍스트 할당 */
	if (!ctx) {
		SPDK_ERRLOG("Unable to allocate context\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	for (i = 1; i <= subsystem->max_nsid; i++) {
		if (anagrpid == 0 || i == anagrpid) {
			/* [한국어] anagrpid=0이면 모든 그룹, 아니면 해당 그룹만 상태 업데이트 */
			listener->ana_state[i - 1] = ana_state;
		}
	}
	listener->ana_state_change_count++;
	/* [한국어] ANA 상태 변경 카운터 증가 — IDENTIFY CONTROLLER 응답에서 변경 감지용 */

	ctx->listener = listener;	/* [한국어] ctx에 변경된 listener 설정 */
	ctx->cb_fn = cb_fn;		/* [한국어] 완료 콜백 설정 */
	ctx->cb_arg = cb_arg;		/* [한국어] 완료 콜백 인자 설정 */

	spdk_for_each_channel(subsystem->tgt,
			      subsystem_listener_update_on_pg,
			      ctx,
			      subsystem_listener_update_done);
	/* [한국어] 모든 poll group에 ANA Change AER 발송 (비동기) — 완료 시 cb_fn 호출 */
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_get_ana_state - 특정 listener의 특정 ANA 그룹 상태 조회
 *
 * @subsystem:  대상 subsystem
 * @trid:       대상 listener의 transport id
 * @anagrpid:   조회할 ANA 그룹 id (1-based, 유효 범위: 1~max_nsid)
 * @ana_state:  조회 결과를 저장할 포인터
 * @return: 0=성공, -EINVAL=유효하지 않은 인자
 *
 * ANA reporting이 활성화된 subsystem에서만 사용 가능.
 * anagrpid=0은 유효하지 않음 (모든 그룹 조회는 지원하지 않음).
 *
 * 호출자: RPC nvmf_get_subsystems 등 상태 조회.
 */
int
spdk_nvmf_subsystem_get_ana_state(struct spdk_nvmf_subsystem *subsystem,
				  const struct spdk_nvme_transport_id *trid,
				  uint32_t anagrpid,
				  enum spdk_nvme_ana_state *ana_state)
{
	assert(ana_state != NULL);	/* [한국어] 결과 저장 포인터는 반드시 유효해야 함 */

	struct spdk_nvmf_subsystem_listener *listener;	/* [한국어] 대상 listener */

	if (!subsystem->flags.ana_reporting) {
		/* [한국어] ANA reporting 비활성화 — 유효하지 않음 */
		SPDK_ERRLOG("ANA reporting is disabled\n");
		return -EINVAL;
	}

	if (anagrpid <= 0 || anagrpid > subsystem->max_nsid) {
		/* [한국어] anagrpid 범위 검사: 1~max_nsid만 유효 */
		SPDK_ERRLOG("ANA group ID %" PRIu32 " is invalid\n", anagrpid);
		return -EINVAL;
	}

	listener = nvmf_subsystem_find_listener(subsystem, trid);
	/* [한국어] trid로 listener 조회 */
	if (!listener) {
		SPDK_ERRLOG("Unable to find listener.\n");
		return -EINVAL;
	}

	*ana_state = listener->ana_state[anagrpid - 1];
	/* [한국어] 1-based anagrpid를 0-based 배열 인덱스로 변환하여 상태 반환 */
	return 0;
}

/*
 * [한국어]
 * spdk_nvmf_subsystem_is_discovery - subsystem이 discovery subsystem인지 확인
 *
 * @subsystem: 대상 subsystem
 * @return: true이면 discovery subsystem
 *
 * Discovery subsystem: NVMe-oF spec §7.4에 따라 대상 NQN을 광고(advertise)하는 역할.
 * DISCOVERY_CURRENT(최신 spec)와 DISCOVERY(구 호환)를 모두 처리.
 *
 * 호출자: nvmf_ctrlr_identify, RPC 조회, spdk_nvmf_tgt_find_subsystem 등.
 */
bool
spdk_nvmf_subsystem_is_discovery(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT ||
	       subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY;
	/* [한국어] DISCOVERY_CURRENT 또는 DISCOVERY(구 호환) subtype이면 discovery subsystem */
}

/*
 * [한국어]
 * nvmf_nqn_is_discovery - NQN이 discovery NQN인지 확인
 *
 * @nqn: 확인할 NQN 문자열
 * @return: SPDK_NVMF_DISCOVERY_NQN과 일치하면 true
 *
 * SPDK_NVMF_DISCOVERY_NQN = "nqn.2014-08.org.nvmexpress.discovery"
 * 호스트가 연결 요청할 때 대상 NQN이 이 값이면 discovery 연결로 처리.
 *
 * 호출자: nvmf_ctrlr_connect 등 연결 처리 시 subsystem 타입 판별.
 */
bool
nvmf_nqn_is_discovery(const char *nqn)
{
	return strcmp(nqn, SPDK_NVMF_DISCOVERY_NQN) == 0;
	/* [한국어] 표준 discovery NQN과 문자열 비교 */
}
