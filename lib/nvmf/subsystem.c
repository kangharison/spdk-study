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

static void
nvmf_host_free(struct spdk_nvmf_host *host)
{
	spdk_keyring_put_key(host->dhchap_key);
	spdk_keyring_put_key(host->dhchap_ctrlr_key);
	free(host);
}

/* Must hold subsystem->mutex while calling this function */
static void
nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_host *host)
{
	TAILQ_REMOVE(&subsystem->hosts, host, link);
	nvmf_host_free(host);
}

static void
_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
				struct spdk_nvmf_subsystem_listener *listener,
				bool stop)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_ctrlr *ctrlr;

	if (stop) {
		assert(nvmf_subsystem_listener_is_active(listener));

		transport = spdk_nvmf_tgt_get_transport(subsystem->tgt, listener->trid->trstring);
		if (transport != NULL) {
			spdk_nvmf_transport_stop_listen(transport, listener->trid);
		}
	}

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		if (ctrlr->listener == listener) {
			ctrlr->listener = NULL;
		}
	}

	TAILQ_REMOVE(&subsystem->listeners, listener, link);
	if (spdk_nvmf_subsystem_is_discovery(listener->subsystem)) {
		nvmf_tgt_update_mdns_prr(listener->subsystem->tgt);
	}
	spdk_nvmf_send_discovery_log_notice(listener->subsystem->tgt, NULL);
	free(listener->ana_state);
	spdk_bit_array_clear(subsystem->used_listener_ids, listener->id);
	free(listener->opts.sock_impl);
	free(listener);
}

static void
_nvmf_subsystem_destroy_msg(void *cb_arg)
{
	struct spdk_nvmf_subsystem *subsystem = cb_arg;

	_nvmf_subsystem_destroy(subsystem);
}

static int
_nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem)
{
	struct nvmf_subsystem_state_change_ctx *ctx;
	struct spdk_nvmf_ns		*ns;
	nvmf_subsystem_destroy_cb	async_destroy_cb = NULL;
	void				*async_destroy_cb_arg = NULL;

	if (!TAILQ_EMPTY(&subsystem->ctrlrs)) {
		SPDK_DEBUGLOG(nvmf, "subsystem %p %s has active controllers\n", subsystem, subsystem->subnqn);
		subsystem->async_destroy = true;
		spdk_thread_send_msg(subsystem->thread, _nvmf_subsystem_destroy_msg, subsystem);
		return -EINPROGRESS;
	}

	ns = spdk_nvmf_subsystem_get_first_ns(subsystem);
	while (ns != NULL) {
		struct spdk_nvmf_ns *next_ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns);

		spdk_nvmf_subsystem_remove_ns(subsystem, ns->opts.nsid);
		ns = next_ns;
	}

	while ((ctx = TAILQ_FIRST(&subsystem->state_changes))) {
		SPDK_WARNLOG("subsystem %s has pending state change requests\n", subsystem->subnqn);
		TAILQ_REMOVE(&subsystem->state_changes, ctx, link);
		if (ctx->cb_fn != NULL) {
			ctx->cb_fn(subsystem, ctx->cb_arg, -ECANCELED);
		}
		free(ctx);
	}

	free(subsystem->ns);
	free(subsystem->ana_group);

	RB_REMOVE(subsystem_tree, &subsystem->tgt->subsystems, subsystem);
	assert(spdk_bit_array_get(subsystem->tgt->subsystem_ids, subsystem->id) == true);
	spdk_bit_array_clear(subsystem->tgt->subsystem_ids, subsystem->id);

	pthread_mutex_destroy(&subsystem->mutex);

	spdk_bit_array_free(&subsystem->used_listener_ids);

	if (subsystem->async_destroy) {
		async_destroy_cb = subsystem->async_destroy_cb;
		async_destroy_cb_arg = subsystem->async_destroy_cb_arg;
	}

	free(subsystem);

	if (async_destroy_cb) {
		async_destroy_cb(async_destroy_cb_arg);
	}

	return 0;
}

static struct spdk_nvmf_ns *
_nvmf_subsystem_get_first_zoned_ns(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_ns *ns = spdk_nvmf_subsystem_get_first_ns(subsystem);
	while (ns != NULL) {
		if (ns->csi == SPDK_NVME_CSI_ZNS) {
			return ns;
		}
		ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns);
	}
	return NULL;
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

static spdk_nvmf_subsystem_state_t
nvmf_subsystem_get_intermediate_state(enum spdk_nvmf_subsystem_state current_state,
				      enum spdk_nvmf_subsystem_state requested_state)
{
	switch (requested_state) {
	case SPDK_NVMF_SUBSYSTEM_INACTIVE:
		return SPDK_NVMF_SUBSYSTEM_DEACTIVATING;
	case SPDK_NVMF_SUBSYSTEM_ACTIVE:
		if (current_state == SPDK_NVMF_SUBSYSTEM_PAUSED) {
			return SPDK_NVMF_SUBSYSTEM_RESUMING;
		} else {
			return SPDK_NVMF_SUBSYSTEM_ACTIVATING;
		}
	case SPDK_NVMF_SUBSYSTEM_PAUSED:
		return SPDK_NVMF_SUBSYSTEM_PAUSING;
	default:
		assert(false);
		return SPDK_NVMF_SUBSYSTEM_NUM_STATES;
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

static void
_nvmf_subsystem_state_change_complete(void *_ctx)
{
	struct nvmf_subsystem_state_change_ctx *next, *ctx = _ctx;
	struct spdk_nvmf_subsystem *subsystem = ctx->subsystem;

	pthread_mutex_lock(&subsystem->mutex);
	assert(TAILQ_FIRST(&subsystem->state_changes) == ctx);
	TAILQ_REMOVE(&subsystem->state_changes, ctx, link);
	next = TAILQ_FIRST(&subsystem->state_changes);
	pthread_mutex_unlock(&subsystem->mutex);

	if (ctx->cb_fn != NULL) {
		ctx->cb_fn(subsystem, ctx->cb_arg, ctx->status);
	}
	free(ctx);

	if (next != NULL) {
		nvmf_subsystem_do_state_change(next);
	}
}

static void
nvmf_subsystem_state_change_complete(struct nvmf_subsystem_state_change_ctx *ctx, int status)
{
	ctx->status = status;
	spdk_thread_exec_msg(ctx->thread, _nvmf_subsystem_state_change_complete, ctx);
}

static void
subsystem_state_change_revert_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_subsystem_state_change_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	/* Nothing to be done here if the state setting fails, we are just screwed. */
	if (nvmf_subsystem_set_state(ctx->subsystem, ctx->requested_state)) {
		SPDK_ERRLOG("Unable to revert the subsystem state after operation failure.\n");
	}

	/* return a failure here. This function only exists in an error path. */
	nvmf_subsystem_state_change_complete(ctx, -1);
}

static void
subsystem_state_change_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_subsystem_state_change_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	enum spdk_nvmf_subsystem_state intermediate_state;

	SPDK_DTRACE_PROBE4(nvmf_subsystem_change_state_done, ctx->subsystem->subnqn,
			   ctx->requested_state, ctx->original_state, status);

	if (status == 0) {
		status = nvmf_subsystem_set_state(ctx->subsystem, ctx->requested_state);
		if (status) {
			status = -1;
		}
	}

	if (status) {
		intermediate_state = nvmf_subsystem_get_intermediate_state(ctx->requested_state,
				     ctx->original_state);
		assert(intermediate_state != SPDK_NVMF_SUBSYSTEM_NUM_STATES);

		if (nvmf_subsystem_set_state(ctx->subsystem, intermediate_state)) {
			goto out;
		}
		ctx->requested_state = ctx->original_state;
		spdk_for_each_channel(ctx->subsystem->tgt,
				      subsystem_state_change_on_pg,
				      ctx,
				      subsystem_state_change_revert_done);
		return;
	}

out:
	nvmf_subsystem_state_change_complete(ctx, status);
}

static void
subsystem_state_change_continue(void *ctx, int status)
{
	struct spdk_io_channel_iter *i = ctx;
	struct nvmf_subsystem_state_change_ctx *_ctx __attribute__((unused));

	_ctx = spdk_io_channel_iter_get_ctx(i);
	SPDK_DTRACE_PROBE3(nvmf_pg_change_state_done, _ctx->subsystem->subnqn,
			   _ctx->requested_state, spdk_thread_get_id(spdk_get_thread()));

	spdk_for_each_channel_continue(i, status);
}

static void
subsystem_state_change_on_pg(struct spdk_io_channel_iter *i)
{
	struct nvmf_subsystem_state_change_ctx *ctx;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;

	ctx = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	SPDK_DTRACE_PROBE3(nvmf_pg_change_state, ctx->subsystem->subnqn,
			   ctx->requested_state, spdk_thread_get_id(spdk_get_thread()));
	switch (ctx->requested_state) {
	case SPDK_NVMF_SUBSYSTEM_INACTIVE:
		nvmf_poll_group_remove_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		break;
	case SPDK_NVMF_SUBSYSTEM_ACTIVE:
		if (ctx->subsystem->state == SPDK_NVMF_SUBSYSTEM_ACTIVATING) {
			nvmf_poll_group_add_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		} else if (ctx->subsystem->state == SPDK_NVMF_SUBSYSTEM_RESUMING) {
			nvmf_poll_group_resume_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		}
		break;
	case SPDK_NVMF_SUBSYSTEM_PAUSED:
		nvmf_poll_group_pause_subsystem(group, ctx->subsystem, ctx->nsid, subsystem_state_change_continue,
						i);
		break;
	default:
		assert(false);
		break;
	}
}

static void
nvmf_subsystem_do_state_change(struct nvmf_subsystem_state_change_ctx *ctx)
{
	struct spdk_nvmf_subsystem *subsystem = ctx->subsystem;
	enum spdk_nvmf_subsystem_state intermediate_state;
	int rc;

	SPDK_DTRACE_PROBE3(nvmf_subsystem_change_state, subsystem->subnqn,
			   ctx->requested_state, subsystem->state);

	/* If we are already in the requested state, just call the callback immediately. */
	if (subsystem->state == ctx->requested_state) {
		nvmf_subsystem_state_change_complete(ctx, 0);
		return;
	}

	intermediate_state = nvmf_subsystem_get_intermediate_state(subsystem->state,
			     ctx->requested_state);
	assert(intermediate_state != SPDK_NVMF_SUBSYSTEM_NUM_STATES);

	ctx->original_state = subsystem->state;
	rc = nvmf_subsystem_set_state(subsystem, intermediate_state);
	if (rc) {
		nvmf_subsystem_state_change_complete(ctx, -1);
		return;
	}

	spdk_for_each_channel(subsystem->tgt,
			      subsystem_state_change_on_pg,
			      ctx,
			      subsystem_state_change_done);
}


static int
nvmf_subsystem_state_change(struct spdk_nvmf_subsystem *subsystem,
			    uint32_t nsid,
			    enum spdk_nvmf_subsystem_state requested_state,
			    spdk_nvmf_subsystem_state_change_done cb_fn,
			    void *cb_arg)
{
	struct nvmf_subsystem_state_change_ctx *ctx;
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (thread == NULL) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->subsystem = subsystem;
	ctx->nsid = nsid;
	ctx->requested_state = requested_state;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->thread = thread;

	pthread_mutex_lock(&subsystem->mutex);
	TAILQ_INSERT_TAIL(&subsystem->state_changes, ctx, link);
	if (ctx != TAILQ_FIRST(&subsystem->state_changes)) {
		pthread_mutex_unlock(&subsystem->mutex);
		return 0;
	}
	pthread_mutex_unlock(&subsystem->mutex);

	nvmf_subsystem_do_state_change(ctx);

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

struct spdk_nvmf_subsystem *
spdk_nvmf_subsystem_get_first(struct spdk_nvmf_tgt *tgt)
{
	return RB_MIN(subsystem_tree, &tgt->subsystems);
}

struct spdk_nvmf_subsystem *
spdk_nvmf_subsystem_get_next(struct spdk_nvmf_subsystem *subsystem)
{
	if (!subsystem) {
		return NULL;
	}

	return RB_NEXT(subsystem_tree, &tgt->subsystems, subsystem);
}

static int
nvmf_ns_add_host(struct spdk_nvmf_ns *ns, const char *hostnqn)
{
	struct spdk_nvmf_host *host;

	host = calloc(1, sizeof(*host));
	if (!host) {
		return -ENOMEM;
	}
	snprintf(host->nqn, sizeof(host->nqn), "%s", hostnqn);
	TAILQ_INSERT_HEAD(&ns->hosts, host, link);
	return 0;
}

static void
nvmf_ns_remove_host(struct spdk_nvmf_ns *ns, struct spdk_nvmf_host *host)
{
	TAILQ_REMOVE(&ns->hosts, host, link);
	free(host);
}

static void
_async_event_ns_notice(void *_ctrlr)
{
	struct spdk_nvmf_ctrlr *ctrlr = _ctrlr;

	nvmf_ctrlr_async_event_ns_notice(ctrlr);
}

static void
send_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	spdk_thread_send_msg(ctrlr->thread, _async_event_ns_notice, ctrlr);
}

static int
nvmf_ns_visible(struct spdk_nvmf_subsystem *subsystem,
		uint32_t nsid,
		const char *hostnqn,
		bool visible)
{
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_host *host;
	int rc;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		assert(false);
		return -1;
	}

	if (hostnqn == NULL || !nvmf_nqn_is_valid(hostnqn)) {
		return -EINVAL;
	}

	if (nsid == 0 || nsid > subsystem->max_nsid) {
		return -EINVAL;
	}

	ns = subsystem->ns[nsid - 1];
	if (!ns) {
		return -ENOENT;
	}

	if (ns->always_visible) {
		/* No individual host control */
		return -EPERM;
	}

	/* Save host info to use for any future controllers. */
	host = nvmf_ns_find_host(ns, hostnqn);
	if (visible && host == NULL) {
		rc = nvmf_ns_add_host(ns, hostnqn);
		if (rc) {
			return rc;
		}
	} else if (!visible && host != NULL) {
		nvmf_ns_remove_host(ns, host);
	}

	/* Also apply to existing controllers. */
	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		if (strcmp(hostnqn, ctrlr->hostnqn) ||
		    nvmf_ctrlr_ns_is_visible(ctrlr, nsid) == visible) {
			continue;
		}
		nvmf_ctrlr_ns_set_visible(ctrlr, nsid, visible);
		send_async_event_ns_notice(ctrlr);
		nvmf_ctrlr_ns_changed(ctrlr, nsid);
	}

	return 0;
}

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
	return nvmf_ns_visible(subsystem, nsid, hostnqn, true);
}

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
	return nvmf_ns_visible(subsystem, nsid, hostnqn, false);
}

/* Must hold subsystem->mutex while calling this function */
static struct spdk_nvmf_host *
nvmf_subsystem_find_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host = NULL;

	TAILQ_FOREACH(host, &subsystem->hosts, link) {
		if (strcmp(hostnqn, host->nqn) == 0) {
			return host;
		}
	}

	return NULL;
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

int
spdk_nvmf_subsystem_set_keys(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn,
			     struct spdk_nvmf_subsystem_key_opts *opts)
{
	struct spdk_nvmf_host *host;
	struct spdk_key *key, *ckey;

	if (!nvmf_auth_is_supported()) {
		SPDK_ERRLOG("NVMe in-band authentication is unsupported\n");
		return -EINVAL;
	}

	pthread_mutex_lock(&subsystem->mutex);
	host = nvmf_subsystem_find_host(subsystem, hostnqn);
	if (host == NULL) {
		pthread_mutex_unlock(&subsystem->mutex);
		return -EINVAL;
	}

	if (SPDK_GET_FIELD(opts, dhchap_key, host->dhchap_key) == NULL &&
	    SPDK_GET_FIELD(opts, dhchap_ctrlr_key, host->dhchap_ctrlr_key) != NULL) {
		SPDK_ERRLOG("DH-HMAC-CHAP controller key requires host key to be set\n");
		pthread_mutex_unlock(&subsystem->mutex);
		return -EINVAL;
	}
	key = SPDK_GET_FIELD(opts, dhchap_key, NULL);
	if (key != NULL) {
		key = spdk_key_dup(key);
		if (key == NULL) {
			pthread_mutex_unlock(&subsystem->mutex);
			return -EINVAL;
		}
	}
	ckey = SPDK_GET_FIELD(opts, dhchap_ctrlr_key, NULL);
	if (ckey != NULL) {
		ckey = spdk_key_dup(ckey);
		if (ckey == NULL) {
			pthread_mutex_unlock(&subsystem->mutex);
			spdk_keyring_put_key(key);
			return -EINVAL;
		}
	}
	if (SPDK_FIELD_VALID(opts, dhchap_key)) {
		spdk_keyring_put_key(host->dhchap_key);
		host->dhchap_key = key;
	}
	if (SPDK_FIELD_VALID(opts, dhchap_ctrlr_key)) {
		spdk_keyring_put_key(host->dhchap_ctrlr_key);
		host->dhchap_ctrlr_key = ckey;
	}
	pthread_mutex_unlock(&subsystem->mutex);

	return 0;
}

struct nvmf_subsystem_disconnect_host_ctx {
	struct spdk_nvmf_subsystem		*subsystem;
	char					*hostnqn;
	spdk_nvmf_tgt_subsystem_listen_done_fn	cb_fn;
	void					*cb_arg;
};

static void
nvmf_subsystem_disconnect_host_fini(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_subsystem_disconnect_host_ctx *ctx;

	ctx = spdk_io_channel_iter_get_ctx(i);

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, status);
	}
	free(ctx->hostnqn);
	free(ctx);
}

static void
nvmf_subsystem_disconnect_qpairs_by_host(struct spdk_io_channel_iter *i)
{
	struct nvmf_subsystem_disconnect_host_ctx *ctx;
	struct spdk_nvmf_poll_group *group;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_qpair *qpair, *tmp_qpair;
	struct spdk_nvmf_ctrlr *ctrlr;

	ctx = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, tmp_qpair) {
		ctrlr = qpair->ctrlr;

		if (ctrlr == NULL || ctrlr->subsys != ctx->subsystem) {
			continue;
		}

		if (strncmp(ctrlr->hostnqn, ctx->hostnqn, sizeof(ctrlr->hostnqn)) == 0) {
			/* Right now this does not wait for the queue pairs to actually disconnect. */
			spdk_nvmf_qpair_disconnect(qpair);
		}
	}
	spdk_for_each_channel_continue(i, 0);
}

int
spdk_nvmf_subsystem_disconnect_host(struct spdk_nvmf_subsystem *subsystem,
				    const char *hostnqn,
				    spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				    void *cb_arg)
{
	struct nvmf_subsystem_disconnect_host_ctx *ctx;

	ctx = calloc(1, sizeof(struct nvmf_subsystem_disconnect_host_ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->hostnqn = strdup(hostnqn);
	if (ctx->hostnqn == NULL) {
		free(ctx);
		return -ENOMEM;
	}

	ctx->subsystem = subsystem;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(subsystem->tgt, nvmf_subsystem_disconnect_qpairs_by_host, ctx,
			      nvmf_subsystem_disconnect_host_fini);

	return 0;
}

int
spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem, bool allow_any_host)
{
	if (subsystem->allow_any_host == allow_any_host) {
		return 0;
	}

	pthread_mutex_lock(&subsystem->mutex);
	subsystem->allow_any_host = allow_any_host;
	if (!TAILQ_EMPTY(&subsystem->listeners)) {
		spdk_nvmf_send_discovery_log_notice(subsystem->tgt, NULL);
	}
	pthread_mutex_unlock(&subsystem->mutex);

	return 0;
}

bool
spdk_nvmf_subsystem_get_allow_any_host(const struct spdk_nvmf_subsystem *subsystem)
{
	bool allow_any_host;
	struct spdk_nvmf_subsystem *sub;

	/* Technically, taking the mutex modifies data in the subsystem. But the const
	 * is still important to convey that this doesn't mutate any other data. Cast
	 * it away to work around this. */
	sub = (struct spdk_nvmf_subsystem *)subsystem;

	pthread_mutex_lock(&sub->mutex);
	allow_any_host = sub->allow_any_host;
	pthread_mutex_unlock(&sub->mutex);

	return allow_any_host;
}

bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	bool allowed;

	if (!hostnqn) {
		return false;
	}

	pthread_mutex_lock(&subsystem->mutex);

	if (subsystem->allow_any_host) {
		pthread_mutex_unlock(&subsystem->mutex);
		return true;
	}

	allowed =  nvmf_subsystem_find_host(subsystem, hostnqn) != NULL;
	pthread_mutex_unlock(&subsystem->mutex);

	return allowed;
}

bool
nvmf_subsystem_host_auth_required(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host;
	bool status;

	pthread_mutex_lock(&subsystem->mutex);
	host = nvmf_subsystem_find_host(subsystem, hostnqn);
	status = host != NULL && host->dhchap_key != NULL;
	pthread_mutex_unlock(&subsystem->mutex);

	return status;
}

struct spdk_key *
nvmf_subsystem_get_dhchap_key(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn,
			      enum nvmf_auth_key_type type)
{
	struct spdk_nvmf_host *host;
	struct spdk_key *key = NULL;

	pthread_mutex_lock(&subsystem->mutex);
	host = nvmf_subsystem_find_host(subsystem, hostnqn);
	if (host != NULL) {
		switch (type) {
		case NVMF_AUTH_KEY_HOST:
			key = host->dhchap_key;
			break;
		case NVMF_AUTH_KEY_CTRLR:
			key = host->dhchap_ctrlr_key;
			break;
		}
		if (key != NULL) {
			key = spdk_key_dup(key);
		}
	}
	pthread_mutex_unlock(&subsystem->mutex);

	return key;
}

struct spdk_nvmf_host *
spdk_nvmf_subsystem_get_first_host(struct spdk_nvmf_subsystem *subsystem)
{
	return TAILQ_FIRST(&subsystem->hosts);
}


struct spdk_nvmf_host *
spdk_nvmf_subsystem_get_next_host(struct spdk_nvmf_subsystem *subsystem,
				  struct spdk_nvmf_host *prev_host)
{
	return TAILQ_NEXT(prev_host, link);
}

const char *
spdk_nvmf_host_get_nqn(const struct spdk_nvmf_host *host)
{
	return host->nqn;
}

struct spdk_nvmf_subsystem_listener *
nvmf_subsystem_find_listener(struct spdk_nvmf_subsystem *subsystem,
			     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_subsystem_listener *listener;

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (!nvmf_subsystem_listener_is_active(listener)) {
			continue;
		}

		if (spdk_nvme_transport_id_compare(listener->trid, trid) == 0) {
			return listener;
		}
	}

	return NULL;
}

bool
nvmf_subsystem_listener_is_active(const struct spdk_nvmf_subsystem_listener *listener)
{
	if (!listener) {
		return false;
	}

	/* Listener was stopped. */
	if (!listener->trid) {
		return false;
	}

	return true;
}

/**
 * Function to be called once the target is listening.
 *
 * \param ctx Context argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
static void
_nvmf_subsystem_add_listener_done(void *ctx, int status)
{
	struct spdk_nvmf_subsystem_listener *listener = ctx;
	struct spdk_nvmf_subsystem *subsystem = listener->subsystem;

	if (status) {
		goto done;
	}

	TAILQ_INSERT_HEAD(&subsystem->listeners, listener, link);

	if (spdk_nvmf_subsystem_is_discovery(subsystem)) {
		status = nvmf_tgt_update_mdns_prr(subsystem->tgt);
		if (status) {
			TAILQ_REMOVE(&subsystem->listeners, listener, link);
			goto done;
		}
	}

	SPDK_DTRACE_PROBE4(nvmf_subsystem_add_listener, subsystem->subnqn, listener->trid->trtype,
			   listener->trid->traddr, listener->trid->trsvcid);

	spdk_nvmf_send_discovery_log_notice(subsystem->tgt, NULL);

done:
	listener->cb_fn(listener->cb_arg, status);
	if (status) {
		free(listener->ana_state);
		free(listener->opts.sock_impl);
		free(listener);
	}
}

void
spdk_nvmf_subsystem_listener_opts_init(struct spdk_nvmf_listener_opts *opts, size_t size)
{
	if (opts == NULL) {
		SPDK_ERRLOG("opts should not be NULL\n");
		assert(false);
		return;
	}
	if (size == 0) {
		SPDK_ERRLOG("size should not be zero\n");
		assert(false);
		return;
	}

	memset(opts, 0, size);
	opts->opts_size = size;

#define FIELD_OK(field) \
	offsetof(struct spdk_nvmf_listener_opts, field) + sizeof(opts->field) <= size

#define SET_FIELD(field, value) \
	if (FIELD_OK(field)) { \
		opts->field = value; \
	} \

	SET_FIELD(secure_channel, false);
	SET_FIELD(ana_state, SPDK_NVME_ANA_OPTIMIZED_STATE);
	SET_FIELD(sock_impl, NULL);

#undef FIELD_OK
#undef SET_FIELD
}

static int
listener_opts_copy(struct spdk_nvmf_listener_opts *src, struct spdk_nvmf_listener_opts *dst)
{
	if (src->opts_size == 0) {
		SPDK_ERRLOG("source structure size should not be zero\n");
		assert(false);
		return -EINVAL;
	}

	memset(dst, 0, sizeof(*dst));
	dst->opts_size = src->opts_size;

#define FIELD_OK(field) \
	offsetof(struct spdk_nvmf_listener_opts, field) + sizeof(src->field) <= src->opts_size

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		dst->field = src->field; \
	} \

	SET_FIELD(secure_channel);
	SET_FIELD(ana_state);
	SET_FIELD(sock_impl);
	/* We should not remove this statement, but need to update the assert statement
	 * if we add a new field, and also add a corresponding SET_FIELD statement. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_listener_opts) == 24, "Incorrect size");

#undef SET_FIELD
#undef FIELD_OK

	return 0;
}

static void
_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
			     const struct spdk_nvme_transport_id *trid,
			     spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
			     void *cb_arg, struct spdk_nvmf_listener_opts *opts)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_subsystem_listener *listener;
	struct spdk_nvmf_listener *tr_listener;
	uint32_t i;
	uint32_t id;
	int rc = 0;

	assert(cb_fn != NULL);

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		cb_fn(cb_arg, -EAGAIN);
		return;
	}

	if (nvmf_subsystem_find_listener(subsystem, trid)) {
		/* Listener already exists in this subsystem */
		cb_fn(cb_arg, 0);
		return;
	}

	transport = spdk_nvmf_tgt_get_transport(subsystem->tgt, trid->trstring);
	if (!transport) {
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    trid->trstring);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	tr_listener = nvmf_transport_find_listener(transport, trid);
	if (!tr_listener) {
		SPDK_ERRLOG("Cannot find transport listener for %s\n", trid->traddr);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	listener = calloc(1, sizeof(*listener));
	if (!listener) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	listener->trid = &tr_listener->trid;
	listener->transport = transport;
	listener->cb_fn = cb_fn;
	listener->cb_arg = cb_arg;
	listener->subsystem = subsystem;
	listener->ana_state = calloc(subsystem->max_nsid, sizeof(enum spdk_nvme_ana_state));
	if (!listener->ana_state) {
		_nvmf_subsystem_add_listener_done(listener, -ENOMEM);
		return;
	}

	spdk_nvmf_subsystem_listener_opts_init(&listener->opts, sizeof(listener->opts));
	if (opts != NULL) {
		rc = listener_opts_copy(opts, &listener->opts);
		if (rc) {
			SPDK_ERRLOG("Unable to copy listener options\n");
			_nvmf_subsystem_add_listener_done(listener, -EINVAL);
			return;
		}
	}

	id = spdk_bit_array_find_first_clear(subsystem->used_listener_ids, 0);
	if (id == UINT32_MAX) {
		SPDK_ERRLOG("Cannot add any more listeners\n");
		_nvmf_subsystem_add_listener_done(listener, -EINVAL);
		return;
	}

	spdk_bit_array_set(subsystem->used_listener_ids, id);
	listener->id = id;

	for (i = 0; i < subsystem->max_nsid; i++) {
		listener->ana_state[i] = listener->opts.ana_state;
	}

	if (transport->ops->listen_associate != NULL) {
		rc = transport->ops->listen_associate(transport, subsystem, trid);
		if (rc) {
			SPDK_ERRLOG("Associate listener for transport %s failed with rc:%d\n", trid->trstring, rc);
		}
	}

	_nvmf_subsystem_add_listener_done(listener, rc);
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

void
nvmf_subsystem_remove_all_listeners(struct spdk_nvmf_subsystem *subsystem,
				    bool stop)
{
	struct spdk_nvmf_subsystem_listener *listener, *listener_tmp;

	TAILQ_FOREACH_SAFE(listener, &subsystem->listeners, link, listener_tmp) {
		_nvmf_subsystem_remove_listener(subsystem, listener, stop);
	}
}

bool
spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
				     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_subsystem_listener *listener;

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (!nvmf_subsystem_listener_is_active(listener)) {
			continue;
		}

		if (spdk_nvme_transport_id_compare(listener->trid, trid) == 0) {
			return true;
		}
	}

	if (!strcmp(subsystem->subnqn, SPDK_NVMF_DISCOVERY_NQN)) {
		SPDK_WARNLOG("Allowing connection to discovery subsystem on %s/%s/%s, "
			     "even though this listener was not added to the discovery "
			     "subsystem.  This behavior is deprecated and will be removed "
			     "in a future release.\n",
			     spdk_nvme_transport_id_trtype_str(trid->trtype), trid->traddr, trid->trsvcid);
		return true;
	}

	return false;
}

struct spdk_nvmf_subsystem_listener *
spdk_nvmf_subsystem_get_first_listener(struct spdk_nvmf_subsystem *subsystem)
{
	return TAILQ_FIRST(&subsystem->listeners);
}

struct spdk_nvmf_subsystem_listener *
spdk_nvmf_subsystem_get_next_listener(struct spdk_nvmf_subsystem *subsystem,
				      struct spdk_nvmf_subsystem_listener *prev_listener)
{
	return TAILQ_NEXT(prev_listener, link);
}

const struct spdk_nvme_transport_id *
spdk_nvmf_subsystem_listener_get_trid(struct spdk_nvmf_subsystem_listener *listener)
{
	return listener->trid;
}

void
spdk_nvmf_subsystem_allow_any_listener(struct spdk_nvmf_subsystem *subsystem,
				       bool allow_any_listener)
{
	subsystem->flags.allow_any_listener = allow_any_listener;
}

bool
spdk_nvmf_subsystem_any_listener_allowed(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->flags.allow_any_listener;
}

void
nvmf_subsystem_poll_group_update_ns_reservation(const struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_subsystem_pg_ns_info *pg_ns)
{
	uint32_t j;
	struct spdk_nvmf_registrant *reg;

	pg_ns->crkey = ns->crkey;
	pg_ns->rtype = ns->rtype;
	if (ns->holder) {
		pg_ns->holder_id = ns->holder->hostid;
	} else {
		memset(&pg_ns->holder_id, 0, sizeof(pg_ns->holder_id));
	}

	memset(&pg_ns->reg_hostid, 0, SPDK_NVMF_MAX_NUM_REGISTRANTS * sizeof(struct spdk_uuid));
	j = 0;
	TAILQ_FOREACH(reg, &ns->registrants, link) {
		if (j >= SPDK_NVMF_MAX_NUM_REGISTRANTS) {
			SPDK_ERRLOG("Maximum %u registrants can support.\n", SPDK_NVMF_MAX_NUM_REGISTRANTS);
			/* This should never happen as we enforce SPDK_NVMF_MAX_NUM_REGISTRANTS
			 * on ns->registrants, but we don't want to continue with poll groups
			 * missing registrants.
			 */
			abort();
		}
		pg_ns->reg_hostid[j++] = reg->hostid;
	}
}

static bool
ns_reservation_hostid_list_contains_id(const struct spdk_uuid *hostid_list, uint32_t num_hostid,
				       const struct spdk_uuid *id)
{
	size_t i;

	for (i = 0; i < num_hostid; i++) {
		if (!spdk_uuid_compare(&hostid_list[i], id)) {
			return true;
		}
	}
	return false;
}

static bool
ns_reservation_io_should_wait(const struct spdk_nvme_cmd *cmd)
{
	switch (cmd->opc) {
	/* We don't wait on reservation commands that modify state because
	 * those are serialized and will cause a deadlock.
	 */
	case SPDK_NVME_OPC_RESERVATION_REGISTER:
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		return false;
	default:
		return true;
	}
}

static bool
ns_reservation_req_is_preempt_abort(const struct spdk_nvmf_request *req)
{
	const struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	return cmd->opc == SPDK_NVME_OPC_RESERVATION_ACQUIRE &&
	       cmd->cdw10_bits.resv_acquire.racqa == SPDK_NVME_RESERVE_PREEMPT_ABORT;
}

static void
poll_group_reservation_build_io_waiting(const struct spdk_nvmf_poll_group *group,
					const struct spdk_nvmf_subsystem *subsystem, const struct spdk_nvmf_ns *ns,
					const struct spdk_nvmf_request *req, struct spdk_nvmf_subsystem_pg_ns_info *pg_ns)
{
	struct spdk_nvmf_qpair *qpair;
	struct spdk_nvmf_request *q_req;
	struct spdk_nvmf_reservation_preempt_abort_info *p_info = ns->preempt_abort;
	const struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	bool hostid_match;

	pg_ns->preempt_abort.io_waiting = 0;
	if (!p_info->hostids_cnt) {
		/* no preempted hostids */
		return;
	}
	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		if (!qpair->ctrlr || qpair->ctrlr->subsys != subsystem) {
			continue;
		}
		hostid_match = ns_reservation_hostid_list_contains_id(p_info->hostids,
				p_info->hostids_cnt, &qpair->ctrlr->hostid);
		if (!hostid_match) {
			continue;
		}

		/* This is a preempted controller, check for IOs on the same namespace */
		TAILQ_FOREACH(q_req, &qpair->outstanding, link) {
			struct spdk_nvme_cmd *req_cmd = &q_req->cmd->nvme_cmd;
			if (req_cmd->nsid == cmd->nsid && ns_reservation_io_should_wait(req_cmd)) {
				pg_ns->preempt_abort.io_waiting++;
				q_req->reservation_waiting = 1;
			}
		}
	}
}

static void
poll_group_reservation_preempt_abort_process(struct spdk_nvmf_poll_group *group,
		struct spdk_nvmf_ns *ns, struct spdk_nvmf_subsystem_pg_ns_info *pg_ns)
{
	struct spdk_nvmf_request *req;

	/* Check for in-progress reservations to process */
	if (STAILQ_EMPTY(&ns->reservations)) {
		return;
	}
	req = STAILQ_FIRST(&ns->reservations);
	/* Check if this is a preempt-and-abort cmd */
	if (!ns_reservation_req_is_preempt_abort(req)) {
		return;
	}

	/* Ensure we have not already processed this */
	if (ns->preempt_abort->hostids_gen == pg_ns->preempt_abort.hostids_gen) {
		SPDK_ERRLOG("Poll group: %p already processed preempt hostids: %u\n",
			    group, ns->preempt_abort->hostids_gen);
		return;
	}

	if (pg_ns->preempt_abort.io_waiting) {
		/* This could happen if a previous preempt-and-abort failed before
		 * completing the IO waiting. Don't let this block the next abort
		 */
		SPDK_ERRLOG("Poll group: %p has incomplete preempted io waiting: %lu\n",
			    group, pg_ns->preempt_abort.io_waiting);
	}

	poll_group_reservation_build_io_waiting(group, ns->subsystem, ns, req, pg_ns);
	/* Commit gen as processed */
	pg_ns->preempt_abort.hostids_gen = ns->preempt_abort->hostids_gen;
}

static void _nvmf_ns_reservation_update_done(struct spdk_nvmf_subsystem *subsystem,
		void *cb_arg, int status);

static void
ns_reservation_pg_update_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_ns *ns = (struct spdk_nvmf_ns *)spdk_io_channel_iter_get_ctx(i);

	if (status) {
		SPDK_ERRLOG("Poll group reservation updated failed on subsystem: %p, ns: %u\n",
			    ns->subsystem, ns->nsid);
		/*
		 * Errors paths have been eliminated for this poll group update, so
		 * this should never happen but if it does, that means the poll group
		 * reservation state is inconsistent and it's not safe to continue!!
		 */
		abort();
	}

	_nvmf_ns_reservation_update_done(ns->subsystem,
					 STAILQ_FIRST(&ns->reservations), 0);
}

static void
ns_reservation_pg_update(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	struct spdk_nvmf_subsystem_pg_ns_info *pg_ns;

	ns = spdk_io_channel_iter_get_ctx(i);
	group = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));
	sgroup = &group->sgroups[ns->subsystem->id];
	pg_ns = &sgroup->ns_info[ns->nsid - 1];

	nvmf_subsystem_poll_group_update_ns_reservation(ns, pg_ns);
	poll_group_reservation_preempt_abort_process(group, ns, pg_ns);

	spdk_for_each_channel_continue(i, 0);
}

static void
nvmf_subsystem_ns_changed(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_ctrlr *ctrlr;

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		if (nvmf_ctrlr_ns_is_visible(ctrlr, nsid)) {
			nvmf_ctrlr_ns_changed(ctrlr, nsid);
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
	spdk_nvmf_subsystem_state_change_done	cb_fn;
	uint32_t				nsid;
};

static void
_nvmf_ns_hot_remove(struct spdk_nvmf_subsystem *subsystem,
		    void *cb_arg, int status)
{
	struct subsystem_ns_change_ctx *ctx = cb_arg;
	int rc;

	rc = spdk_nvmf_subsystem_remove_ns(subsystem, ctx->nsid);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to make changes to NVME-oF subsystem with id: %u\n", subsystem->id);
	}

	rc = spdk_nvmf_subsystem_resume(subsystem, NULL, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to resume NVME-oF subsystem with id: %u\n", subsystem->id);
	}

	free(ctx);
}

static void
nvmf_ns_change_msg(void *ns_ctx)
{
	struct subsystem_ns_change_ctx *ctx = ns_ctx;
	int rc;

	SPDK_DTRACE_PROBE2(nvmf_ns_change, ctx->nsid, ctx->subsystem->subnqn);

	rc = spdk_nvmf_subsystem_pause(ctx->subsystem, ctx->nsid, ctx->cb_fn, ctx);
	if (rc) {
		if (rc == -EBUSY) {
			/* Try again, this is not a permanent situation. */
			spdk_thread_send_msg(spdk_get_thread(), nvmf_ns_change_msg, ctx);
		} else {
			free(ctx);
			SPDK_ERRLOG("Unable to pause subsystem to process namespace removal!\n");
		}
	}
}

static void
nvmf_ns_hot_remove(void *remove_ctx)
{
	struct spdk_nvmf_ns *ns = remove_ctx;
	struct subsystem_ns_change_ctx *ns_ctx;
	int rc;

	/* We have to allocate a new context because this op
	 * is asynchronous and we could lose the ns in the middle.
	 */
	ns_ctx = calloc(1, sizeof(struct subsystem_ns_change_ctx));
	if (!ns_ctx) {
		SPDK_ERRLOG("Unable to allocate context to process namespace removal!\n");
		return;
	}

	ns_ctx->subsystem = ns->subsystem;
	ns_ctx->nsid = ns->opts.nsid;
	ns_ctx->cb_fn = _nvmf_ns_hot_remove;

	rc = spdk_nvmf_subsystem_pause(ns->subsystem, ns_ctx->nsid, _nvmf_ns_hot_remove, ns_ctx);
	if (rc) {
		if (rc == -EBUSY) {
			/* Try again, this is not a permanent situation. */
			spdk_thread_send_msg(spdk_get_thread(), nvmf_ns_change_msg, ns_ctx);
		} else {
			SPDK_ERRLOG("Unable to pause subsystem to process namespace removal!\n");
			free(ns_ctx);
		}
	}
}

static void
_nvmf_ns_resize(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	struct subsystem_ns_change_ctx *ctx = cb_arg;

	nvmf_subsystem_ns_changed(subsystem, ctx->nsid);
	if (spdk_nvmf_subsystem_resume(subsystem, NULL, NULL) != 0) {
		SPDK_ERRLOG("Failed to resume NVME-oF subsystem with id: %u\n", subsystem->id);
	}

	free(ctx);
}

static void
nvmf_ns_resize(void *event_ctx)
{
	struct spdk_nvmf_ns *ns = event_ctx;
	struct subsystem_ns_change_ctx *ns_ctx;
	int rc;

	/* We have to allocate a new context because this op
	 * is asynchronous and we could lose the ns in the middle.
	 */
	ns_ctx = calloc(1, sizeof(struct subsystem_ns_change_ctx));
	if (!ns_ctx) {
		SPDK_ERRLOG("Unable to allocate context to process namespace removal!\n");
		return;
	}

	ns_ctx->subsystem = ns->subsystem;
	ns_ctx->nsid = ns->opts.nsid;
	ns_ctx->cb_fn = _nvmf_ns_resize;

	/* Specify 0 for the nsid here, because we do not need to pause the namespace.
	 * Namespaces can only be resized bigger, so there is no need to quiesce I/O.
	 */
	rc = spdk_nvmf_subsystem_pause(ns->subsystem, 0, _nvmf_ns_resize, ns_ctx);
	if (rc) {
		if (rc == -EBUSY) {
			/* Try again, this is not a permanent situation. */
			spdk_thread_send_msg(spdk_get_thread(), nvmf_ns_change_msg, ns_ctx);
		} else {
			SPDK_ERRLOG("Unable to pause subsystem to process namespace resize!\n");
			free(ns_ctx);
		}
	}
}

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

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		nvmf_ns_hot_remove(event_ctx);
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		nvmf_ns_resize(event_ctx);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

void
spdk_nvmf_ns_opts_get_defaults(struct spdk_nvmf_ns_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL.\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero.\n");
		return;
	}

	memset(opts, 0, opts_size);
	opts->opts_size = opts_size;

#define FIELD_OK(field) \
	offsetof(struct spdk_nvmf_ns_opts, field) + sizeof(opts->field) <= opts_size

#define SET_FIELD(field, value) \
	if (FIELD_OK(field)) { \
		opts->field = value; \
	} \

	/* All current fields are set to 0 by default. */
	SET_FIELD(nsid, 0);
	if (FIELD_OK(nguid)) {
		memset(opts->nguid, 0, sizeof(opts->nguid));
	}
	if (FIELD_OK(eui64)) {
		memset(opts->eui64, 0, sizeof(opts->eui64));
	}
	if (FIELD_OK(uuid)) {
		spdk_uuid_set_null(&opts->uuid);
	}
	SET_FIELD(anagrpid, 0);
	SET_FIELD(transport_specific, NULL);
	SET_FIELD(hide_metadata, false);

#undef FIELD_OK
#undef SET_FIELD
}

static void
nvmf_ns_opts_copy(struct spdk_nvmf_ns_opts *opts,
		  const struct spdk_nvmf_ns_opts *user_opts,
		  size_t opts_size)
{
#define FIELD_OK(field)	\
	offsetof(struct spdk_nvmf_ns_opts, field) + sizeof(opts->field) <= user_opts->opts_size

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		opts->field = user_opts->field;	\
	} \

	SET_FIELD(nsid);
	if (FIELD_OK(nguid)) {
		memcpy(opts->nguid, user_opts->nguid, sizeof(opts->nguid));
	}
	if (FIELD_OK(eui64)) {
		memcpy(opts->eui64, user_opts->eui64, sizeof(opts->eui64));
	}
	if (FIELD_OK(uuid)) {
		spdk_uuid_copy(&opts->uuid, &user_opts->uuid);
	}
	SET_FIELD(anagrpid);
	SET_FIELD(no_auto_visible);
	SET_FIELD(transport_specific);
	SET_FIELD(hide_metadata);

	opts->opts_size = user_opts->opts_size;

	/* We should not remove this statement, but need to update the assert statement
	 * if we add a new field, and also add a corresponding SET_FIELD statement.
	 */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ns_opts) == 73, "Incorrect size");

#undef FIELD_OK
#undef SET_FIELD
}

/* Dummy bdev module used to to claim bdevs. */
static struct spdk_bdev_module ns_bdev_module = {
	.name	= "NVMe-oF Target",
};

static int nvmf_ns_reservation_update(const struct spdk_nvmf_ns *ns,
				      const struct spdk_nvmf_reservation_info *info);
static int nvmf_ns_reservation_load(const struct spdk_nvmf_ns *ns,
				    struct spdk_nvmf_reservation_info *info);
static int nvmf_ns_reservation_restore(struct spdk_nvmf_ns *ns,
				       struct spdk_nvmf_reservation_info *info);

bool
nvmf_subsystem_zone_append_supported(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_ns *ns;

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem);
	     ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (spdk_bdev_is_zoned(ns->bdev) &&
		    spdk_bdev_io_type_supported(ns->bdev, SPDK_BDEV_IO_TYPE_ZONE_APPEND)) {
			return true;
		}
	}

	return false;
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
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_ns_opts opts;
	struct spdk_bdev_open_opts open_opts = {};
	struct spdk_nvmf_ns *ns, *first_ns;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_reservation_info info = {0};
	int rc;
	bool zone_append_supported;
	uint64_t max_zone_append_size_kib;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return 0;
	}

	spdk_nvmf_ns_opts_get_defaults(&opts, sizeof(opts));
	if (user_opts) {
		nvmf_ns_opts_copy(&opts, user_opts, opts_size);
	}

	if (opts.nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		SPDK_ERRLOG("Invalid NSID %" PRIu32 "\n", opts.nsid);
		return 0;
	}

	if (opts.nsid == 0) {
		/*
		 * NSID not specified - find a free index.
		 *
		 * If no free slots are found, return error.
		 */
		for (opts.nsid = 1; opts.nsid <= subsystem->max_nsid; opts.nsid++) {
			if (_nvmf_subsystem_get_ns(subsystem, opts.nsid) == NULL) {
				break;
			}
		}
		if (opts.nsid > subsystem->max_nsid) {
			SPDK_ERRLOG("No free namespace slot available in the subsystem\n");
			return 0;
		}
	}

	if (opts.nsid > subsystem->max_nsid) {
		SPDK_ERRLOG("NSID greater than maximum not allowed\n");
		return 0;
	}

	if (_nvmf_subsystem_get_ns(subsystem, opts.nsid)) {
		SPDK_ERRLOG("Requested NSID %" PRIu32 " already in use\n", opts.nsid);
		return 0;
	}

	if (opts.anagrpid == 0) {
		opts.anagrpid = opts.nsid;
	}

	if (opts.anagrpid > subsystem->max_nsid) {
		SPDK_ERRLOG("ANAGRPID greater than maximum NSID not allowed\n");
		return 0;
	}

	ns = calloc(1, sizeof(*ns));
	if (ns == NULL) {
		SPDK_ERRLOG("Namespace allocation failed\n");
		return 0;
	}

	TAILQ_INIT(&ns->hosts);
	ns->always_visible = !opts.no_auto_visible;
	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		nvmf_ctrlr_ns_set_visible(ctrlr, opts.nsid, ns->always_visible);
	}

	spdk_bdev_open_opts_init(&open_opts, sizeof(open_opts));
	open_opts.hide_metadata = opts.hide_metadata;

	rc = spdk_bdev_open_ext_v2(bdev_name, true, nvmf_ns_event, ns, &open_opts, &ns->desc);
	if (rc != 0) {
		SPDK_ERRLOG("Subsystem %s: bdev %s cannot be opened, error=%d\n",
			    subsystem->subnqn, bdev_name, rc);
		free(ns);
		return 0;
	}

	ns->bdev = spdk_bdev_desc_get_bdev(ns->desc);

	if (spdk_bdev_desc_get_md_size(ns->desc) != 0) {
		if (!spdk_bdev_desc_is_md_interleaved(ns->desc)) {
			SPDK_ERRLOG("Can't attach bdev with separate metadata.\n");
			spdk_bdev_close(ns->desc);
			free(ns);
			return 0;
		}

		if (spdk_bdev_desc_get_md_size(ns->desc) > SPDK_BDEV_MAX_INTERLEAVED_MD_SIZE) {
			SPDK_ERRLOG("Maximum supported interleaved md size %u, current md size %u\n",
				    SPDK_BDEV_MAX_INTERLEAVED_MD_SIZE,
				    spdk_bdev_desc_get_md_size(ns->desc));
			spdk_bdev_close(ns->desc);
			free(ns);
			return 0;
		}
	}

	rc = spdk_bdev_module_claim_bdev(ns->bdev, ns->desc, &ns_bdev_module);
	if (rc != 0) {
		spdk_bdev_close(ns->desc);
		free(ns);
		return 0;
	}

	ns->passthru_nsid = spdk_bdev_get_nvme_nsid(ns->bdev);
	if (subsystem->passthrough && ns->passthru_nsid == 0) {
		SPDK_ERRLOG("Only bdev_nvme namespaces can be added to a passthrough subsystem.\n");
		goto err;
	}

	/* Cache the zcopy capability of the bdev device */
	ns->zcopy = spdk_bdev_io_type_supported(ns->bdev, SPDK_BDEV_IO_TYPE_ZCOPY);

	if (spdk_uuid_is_null(&opts.uuid)) {
		opts.uuid = *spdk_bdev_get_uuid(ns->bdev);
	}

	/* if nguid descriptor is supported by bdev module (nvme) then uuid = nguid */
	if (spdk_mem_all_zero(opts.nguid, sizeof(opts.nguid))) {
		SPDK_STATIC_ASSERT(sizeof(opts.nguid) == sizeof(opts.uuid), "size mismatch");
		memcpy(opts.nguid, spdk_bdev_get_uuid(ns->bdev), sizeof(opts.nguid));
	}

	if (spdk_bdev_is_zoned(ns->bdev)) {
		SPDK_DEBUGLOG(nvmf, "The added namespace is backed by a zoned block device.\n");
		ns->csi = SPDK_NVME_CSI_ZNS;

		zone_append_supported = spdk_bdev_io_type_supported(ns->bdev,
					SPDK_BDEV_IO_TYPE_ZONE_APPEND);
		max_zone_append_size_kib = spdk_bdev_get_max_zone_append_size(ns->bdev) *
					   spdk_bdev_desc_get_block_size(ns->desc);

		if (_nvmf_subsystem_get_first_zoned_ns(subsystem) != NULL &&
		    (nvmf_subsystem_zone_append_supported(subsystem) != zone_append_supported ||
		     subsystem->max_zone_append_size_kib != max_zone_append_size_kib)) {
			SPDK_ERRLOG("Namespaces with different zone append support or different zone append size are not allowed.\n");
			goto err;
		}

		subsystem->max_zone_append_size_kib = max_zone_append_size_kib;
	}

	first_ns = spdk_nvmf_subsystem_get_first_ns(subsystem);
	if (!first_ns) {
		if (spdk_bdev_get_nvme_ctratt(ns->bdev).bits.fdps) {
			SPDK_DEBUGLOG(nvmf, "Subsystem with id: %u has FDP capability.\n",
				      subsystem->id);
			subsystem->fdp_supported = true;
		}
	} else {
		if (spdk_bdev_get_nvme_ctratt(first_ns->bdev).bits.fdps !=
		    spdk_bdev_get_nvme_ctratt(ns->bdev).bits.fdps) {
			SPDK_ERRLOG("Subsystem with id: %u can%s FDP namespace.\n", subsystem->id,
				    spdk_bdev_get_nvme_ctratt(first_ns->bdev).bits.fdps ? " only add" : "not add");
			goto err;
		}
	}

	ns->opts = opts;
	ns->subsystem = subsystem;
	subsystem->ns[opts.nsid - 1] = ns;
	ns->nsid = opts.nsid;
	ns->anagrpid = opts.anagrpid;
	subsystem->ana_group[ns->anagrpid - 1]++;
	TAILQ_INIT(&ns->registrants);
	STAILQ_INIT(&ns->reservations);
	if (ptpl_file) {
		ns->ptpl_file = strdup(ptpl_file);
		if (!ns->ptpl_file) {
			SPDK_ERRLOG("Namespace ns->ptpl_file allocation failed\n");
			goto err;
		}
	}

	if (nvmf_ns_is_ptpl_capable(ns)) {
		rc = nvmf_ns_reservation_load(ns, &info);
		if (rc) {
			SPDK_ERRLOG("Subsystem load reservation failed\n");
			goto err;
		}

		rc = nvmf_ns_reservation_restore(ns, &info);
		if (rc) {
			SPDK_ERRLOG("Subsystem restore reservation failed\n");
			goto err;
		}
	}

	for (transport = spdk_nvmf_transport_get_first(subsystem->tgt); transport;
	     transport = spdk_nvmf_transport_get_next(transport)) {
		if (transport->ops->subsystem_add_ns) {
			rc = transport->ops->subsystem_add_ns(transport, subsystem, ns);
			if (rc) {
				SPDK_ERRLOG("Namespace attachment is not allowed by %s transport\n", transport->ops->name);
				nvmf_ns_reservation_clear_all_registrants(ns);
				goto err;
			}
		}
	}

	/* JSON value obj is freed before sending the response. Set NULL to prevent usage of dangling pointer. */
	ns->opts.transport_specific = NULL;

	SPDK_DEBUGLOG(nvmf, "Subsystem %s: bdev %s assigned nsid %" PRIu32 "\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem),
		      bdev_name,
		      opts.nsid);

	nvmf_subsystem_ns_changed(subsystem, opts.nsid);

	SPDK_DTRACE_PROBE2(nvmf_subsystem_add_ns, subsystem->subnqn, ns->nsid);

	return opts.nsid;
err:
	subsystem->ns[opts.nsid - 1] = NULL;
	spdk_bdev_module_release_bdev(ns->bdev);
	spdk_bdev_close(ns->desc);
	free(ns->ptpl_file);
	free(ns);

	return 0;
}

int
spdk_nvmf_subsystem_set_ns_ana_group(struct spdk_nvmf_subsystem *subsystem,
				     uint32_t nsid, uint32_t anagrpid)
{
	struct spdk_nvmf_ns *ns;

	if (anagrpid > subsystem->max_nsid) {
		SPDK_ERRLOG("ANAGRPID greater than maximum NSID not allowed\n");
		return -1;
	}

	if (anagrpid == 0) {
		SPDK_ERRLOG("Zero is not allowed to ANAGRPID\n");
		return -1;
	}

	if (nsid == 0 || nsid > subsystem->max_nsid) {
		return -1;
	}

	ns = subsystem->ns[nsid - 1];
	if (!ns) {
		return -1;
	}

	assert(ns->anagrpid - 1 < subsystem->max_nsid);

	assert(subsystem->ana_group[ns->anagrpid - 1] > 0);

	subsystem->ana_group[ns->anagrpid - 1]--;

	subsystem->ana_group[anagrpid - 1]++;

	ns->anagrpid = anagrpid;
	ns->opts.anagrpid = anagrpid;

	nvmf_subsystem_ns_changed(subsystem, nsid);

	return 0;
}

static uint32_t
nvmf_subsystem_get_next_allocated_nsid(struct spdk_nvmf_subsystem *subsystem,
				       uint32_t prev_nsid)
{
	uint32_t nsid;

	if (prev_nsid >= subsystem->max_nsid) {
		return 0;
	}

	for (nsid = prev_nsid + 1; nsid <= subsystem->max_nsid; nsid++) {
		if (subsystem->ns[nsid - 1]) {
			return nsid;
		}
	}

	return 0;
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_first_ns(struct spdk_nvmf_subsystem *subsystem)
{
	uint32_t first_nsid;

	first_nsid = nvmf_subsystem_get_next_allocated_nsid(subsystem, 0);
	return _nvmf_subsystem_get_ns(subsystem, first_nsid);
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem,
				struct spdk_nvmf_ns *prev_ns)
{
	uint32_t next_nsid;

	next_nsid = nvmf_subsystem_get_next_allocated_nsid(subsystem, prev_ns->opts.nsid);
	return _nvmf_subsystem_get_ns(subsystem, next_nsid);
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	return _nvmf_subsystem_get_ns(subsystem, nsid);
}

uint32_t
spdk_nvmf_ns_get_id(const struct spdk_nvmf_ns *ns)
{
	return ns->opts.nsid;
}

struct spdk_bdev *
spdk_nvmf_ns_get_bdev(struct spdk_nvmf_ns *ns)
{
	return ns->bdev;
}

void
spdk_nvmf_ns_get_opts(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_ns_opts *opts,
		      size_t opts_size)
{
	memset(opts, 0, opts_size);
	memcpy(opts, &ns->opts, spdk_min(sizeof(ns->opts), opts_size));
}

const char *
spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->sn;
}

int
spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn)
{
	size_t len, max_len;

	max_len = sizeof(subsystem->sn) - 1;
	len = strlen(sn);
	if (len > max_len) {
		SPDK_DEBUGLOG(nvmf, "Invalid sn \"%s\": length %zu > max %zu\n",
			      sn, len, max_len);
		return -1;
	}

	if (!nvmf_valid_ascii_string(sn, len)) {
		SPDK_DEBUGLOG(nvmf, "Non-ASCII sn\n");
		SPDK_LOGDUMP(nvmf, "sn", sn, len);
		return -1;
	}

	snprintf(subsystem->sn, sizeof(subsystem->sn), "%s", sn);

	return 0;
}

const char *
spdk_nvmf_subsystem_get_mn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->mn;
}

int
spdk_nvmf_subsystem_set_mn(struct spdk_nvmf_subsystem *subsystem, const char *mn)
{
	size_t len, max_len;

	if (mn == NULL) {
		mn = MODEL_NUMBER_DEFAULT;
	}
	max_len = sizeof(subsystem->mn) - 1;
	len = strlen(mn);
	if (len > max_len) {
		SPDK_DEBUGLOG(nvmf, "Invalid mn \"%s\": length %zu > max %zu\n",
			      mn, len, max_len);
		return -1;
	}

	if (!nvmf_valid_ascii_string(mn, len)) {
		SPDK_DEBUGLOG(nvmf, "Non-ASCII mn\n");
		SPDK_LOGDUMP(nvmf, "mn", mn, len);
		return -1;
	}

	snprintf(subsystem->mn, sizeof(subsystem->mn), "%s", mn);

	return 0;
}

const char *
spdk_nvmf_subsystem_get_nqn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subnqn;
}

/* We have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_nvmf_subtype spdk_nvmf_subtype_t;

spdk_nvmf_subtype_t
spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subtype;
}

uint32_t
spdk_nvmf_subsystem_get_max_nsid(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->max_nsid;
}

int
spdk_nvmf_subsystem_set_cntlid_range(struct spdk_nvmf_subsystem *subsystem,
				     uint16_t min_cntlid, uint16_t max_cntlid)
{
	if (subsystem->state != SPDK_NVMF_SUBSYSTEM_INACTIVE) {
		return -EAGAIN;
	}

	if (min_cntlid > max_cntlid) {
		return -EINVAL;
	}
	/* The spec reserves cntlid values in the range FFF0h to FFFFh. */
	if (min_cntlid < NVMF_MIN_CNTLID || min_cntlid > NVMF_MAX_CNTLID ||
	    max_cntlid < NVMF_MIN_CNTLID || max_cntlid > NVMF_MAX_CNTLID) {
		return -EINVAL;
	}
	subsystem->min_cntlid = min_cntlid;
	subsystem->max_cntlid = max_cntlid;
	if (subsystem->next_cntlid < min_cntlid || subsystem->next_cntlid > max_cntlid) {
		subsystem->next_cntlid = min_cntlid;
	}

	return 0;
}

uint16_t
nvmf_subsystem_gen_cntlid(struct spdk_nvmf_subsystem *subsystem)
{
	int count;
	uint16_t cntlid;

	/*
	 * In the worst case, we might have to try all CNTLID values between min_cntlid and max_cntlid
	 * before we find one that is unused (or find that all values are in use).
	 */
	for (count = 0; count < subsystem->max_cntlid - subsystem->min_cntlid + 1; count++) {
		cntlid = subsystem->next_cntlid;
		subsystem->next_cntlid++;

		if (subsystem->next_cntlid > subsystem->max_cntlid) {
			subsystem->next_cntlid = subsystem->min_cntlid;
		}

		/* Check if a controller with this cntlid currently exists. */
		if (nvmf_subsystem_get_ctrlr(subsystem, cntlid) == NULL) {
			/* Found unused cntlid */
			return cntlid;
		}
	}

	/* All valid cntlid values are in use. */
	return 0xFFFF;
}

int
nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr)
{

	if (ctrlr->dynamic_ctrlr) {
		ctrlr->cntlid = nvmf_subsystem_gen_cntlid(subsystem);
		if (ctrlr->cntlid == 0xFFFF) {
			/* Unable to get a cntlid */
			SPDK_ERRLOG("Reached max simultaneous ctrlrs\n");
			return -EBUSY;
		}
	} else if (nvmf_subsystem_get_ctrlr(subsystem, ctrlr->cntlid) != NULL) {
		SPDK_ERRLOG("Ctrlr with cntlid %u already exist\n", ctrlr->cntlid);
		return -EEXIST;
	}

	TAILQ_INSERT_TAIL(&subsystem->ctrlrs, ctrlr, link);

	SPDK_DTRACE_PROBE3(nvmf_subsystem_add_ctrlr, subsystem->subnqn, ctrlr, ctrlr->hostnqn);

	return 0;
}

void
nvmf_subsystem_remove_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			    struct spdk_nvmf_ctrlr *ctrlr)
{
	SPDK_DTRACE_PROBE3(nvmf_subsystem_remove_ctrlr, subsystem->subnqn, ctrlr, ctrlr->hostnqn);

	assert(spdk_get_thread() == subsystem->thread);
	assert(subsystem == ctrlr->subsys);
	SPDK_DEBUGLOG(nvmf, "remove ctrlr %p id 0x%x from subsys %p %s\n", ctrlr, ctrlr->cntlid, subsystem,
		      subsystem->subnqn);
	TAILQ_REMOVE(&subsystem->ctrlrs, ctrlr, link);
}

struct spdk_nvmf_ctrlr *
nvmf_subsystem_get_ctrlr(struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid)
{
	struct spdk_nvmf_ctrlr *ctrlr;

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		if (ctrlr->cntlid == cntlid) {
			return ctrlr;
		}
	}

	return NULL;
}

uint32_t
spdk_nvmf_subsystem_get_max_namespaces(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->max_nsid;
}

uint16_t
spdk_nvmf_subsystem_get_min_cntlid(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->min_cntlid;
}

uint16_t
spdk_nvmf_subsystem_get_max_cntlid(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->max_cntlid;
}

struct _nvmf_ns_registrant {
	uint64_t		rkey;
	char			*host_uuid;
};

struct _nvmf_ns_registrants {
	size_t				num_regs;
	struct _nvmf_ns_registrant	reg[SPDK_NVMF_MAX_NUM_REGISTRANTS];
};

struct _nvmf_ns_reservation {
	bool					ptpl_activated;
	enum spdk_nvme_reservation_type		rtype;
	uint64_t				crkey;
	char					*bdev_uuid;
	char					*holder_uuid;
	struct _nvmf_ns_registrants		regs;
};

static const struct spdk_json_object_decoder nvmf_ns_pr_reg_decoders[] = {
	{"rkey", offsetof(struct _nvmf_ns_registrant, rkey), spdk_json_decode_uint64},
	{"host_uuid", offsetof(struct _nvmf_ns_registrant, host_uuid), spdk_json_decode_string},
};

static int
nvmf_decode_ns_pr_reg(const struct spdk_json_val *val, void *out)
{
	struct _nvmf_ns_registrant *reg = out;

	return spdk_json_decode_object(val, nvmf_ns_pr_reg_decoders,
				       SPDK_COUNTOF(nvmf_ns_pr_reg_decoders), reg);
}

static int
nvmf_decode_ns_pr_regs(const struct spdk_json_val *val, void *out)
{
	struct _nvmf_ns_registrants *regs = out;

	return spdk_json_decode_array(val, nvmf_decode_ns_pr_reg, regs->reg,
				      SPDK_NVMF_MAX_NUM_REGISTRANTS, &regs->num_regs,
				      sizeof(struct _nvmf_ns_registrant));
}

static const struct spdk_json_object_decoder nvmf_ns_pr_decoders[] = {
	{"ptpl", offsetof(struct _nvmf_ns_reservation, ptpl_activated), spdk_json_decode_bool, true},
	{"rtype", offsetof(struct _nvmf_ns_reservation, rtype), spdk_json_decode_uint32, true},
	{"crkey", offsetof(struct _nvmf_ns_reservation, crkey), spdk_json_decode_uint64, true},
	{"bdev_uuid", offsetof(struct _nvmf_ns_reservation, bdev_uuid), spdk_json_decode_string},
	{"holder_uuid", offsetof(struct _nvmf_ns_reservation, holder_uuid), spdk_json_decode_string, true},
	{"registrants", offsetof(struct _nvmf_ns_reservation, regs), nvmf_decode_ns_pr_regs},
};

static int
nvmf_ns_reservation_load_json(const struct spdk_nvmf_ns *ns,
			      struct spdk_nvmf_reservation_info *info)
{
	size_t json_size;
	ssize_t values_cnt, rc;
	void *json = NULL, *end;
	struct spdk_json_val *values = NULL;
	struct _nvmf_ns_reservation res = {};
	const char *file = ns->ptpl_file;
	uint32_t i;

	/* It's not an error if the file does not exist */
	if (access(file, F_OK) != 0) {
		SPDK_DEBUGLOG(nvmf, "File %s does not exist\n", file);
		return 0;
	}

	/* Load all persist file contents into a local buffer */
	json = spdk_posix_file_load_from_name(file, &json_size);
	if (!json) {
		SPDK_ERRLOG("Load persist file %s failed\n", file);
		return -ENOMEM;
	}

	rc = spdk_json_parse(json, json_size, NULL, 0, &end, 0);
	if (rc < 0) {
		SPDK_NOTICELOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto exit;
	}

	values_cnt = rc;
	values = calloc(values_cnt, sizeof(struct spdk_json_val));
	if (values == NULL) {
		goto exit;
	}

	rc = spdk_json_parse(json, json_size, values, values_cnt, &end, 0);
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
		goto exit;
	}

	if (res.regs.num_regs > SPDK_NVMF_MAX_NUM_REGISTRANTS) {
		SPDK_ERRLOG("Can only support up to %u registrants\n", SPDK_NVMF_MAX_NUM_REGISTRANTS);
		rc = -ERANGE;
		goto exit;
	}

	rc = 0;
	info->ptpl_activated = res.ptpl_activated;
	info->rtype = res.rtype;
	info->crkey = res.crkey;
	snprintf(info->bdev_uuid, sizeof(info->bdev_uuid), "%s", res.bdev_uuid);
	snprintf(info->holder_uuid, sizeof(info->holder_uuid), "%s", res.holder_uuid);
	info->num_regs = res.regs.num_regs;
	for (i = 0; i < res.regs.num_regs; i++) {
		info->registrants[i].rkey = res.regs.reg[i].rkey;
		snprintf(info->registrants[i].host_uuid, sizeof(info->registrants[i].host_uuid), "%s",
			 res.regs.reg[i].host_uuid);
	}

exit:
	free(json);
	free(values);
	free(res.bdev_uuid);
	free(res.holder_uuid);
	for (i = 0; i < res.regs.num_regs; i++) {
		free(res.regs.reg[i].host_uuid);
	}

	return rc;
}

static bool nvmf_ns_reservation_all_registrants_type(struct spdk_nvmf_ns *ns);

static int
nvmf_ns_reservation_restore(struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info)
{
	uint32_t i;
	struct spdk_nvmf_registrant *reg, *holder = NULL;
	struct spdk_uuid bdev_uuid, holder_uuid;
	bool rkey_flag = false;

	SPDK_DEBUGLOG(nvmf, "NSID %u, PTPL %u, Number of registrants %u\n",
		      ns->nsid, info->ptpl_activated, info->num_regs);

	/* it's not an error */
	if (!info->ptpl_activated || !info->num_regs) {
		return 0;
	}

	/* Check info->crkey exist or not in info->registrants[i].rkey */
	for (i = 0; i < info->num_regs; i++) {
		if (info->crkey == info->registrants[i].rkey) {
			rkey_flag = true;
		}
	}
	if (!rkey_flag && info->crkey != 0) {
		return -EINVAL;
	}

	spdk_uuid_parse(&bdev_uuid, info->bdev_uuid);
	if (spdk_uuid_compare(&bdev_uuid, spdk_bdev_get_uuid(ns->bdev))) {
		SPDK_ERRLOG("Existing bdev UUID is not same with configuration file\n");
		return -EINVAL;
	}

	ns->crkey = info->crkey;
	ns->rtype = info->rtype;
	ns->ptpl_activated = info->ptpl_activated;
	spdk_uuid_parse(&holder_uuid, info->holder_uuid);

	SPDK_DEBUGLOG(nvmf, "Bdev UUID %s\n", info->bdev_uuid);
	if (info->rtype) {
		SPDK_DEBUGLOG(nvmf, "Holder UUID %s, RTYPE %u, RKEY 0x%"PRIx64"\n",
			      info->holder_uuid, info->rtype, info->crkey);
	}

	for (i = 0; i < info->num_regs; i++) {
		reg = calloc(1, sizeof(*reg));
		if (!reg) {
			return -ENOMEM;
		}
		spdk_uuid_parse(&reg->hostid, info->registrants[i].host_uuid);
		reg->rkey = info->registrants[i].rkey;
		TAILQ_INSERT_TAIL(&ns->registrants, reg, link);
		if (info->crkey != 0 && !spdk_uuid_compare(&holder_uuid, &reg->hostid)) {
			holder = reg;
		}
		SPDK_DEBUGLOG(nvmf, "Registrant RKEY 0x%"PRIx64", Host UUID %s\n",
			      info->registrants[i].rkey, info->registrants[i].host_uuid);
	}

	if (nvmf_ns_reservation_all_registrants_type(ns)) {
		ns->holder = TAILQ_FIRST(&ns->registrants);
	} else {
		ns->holder = holder;
	}

	return 0;
}

static int
nvmf_ns_json_write_cb(void *cb_ctx, const void *data, size_t size)
{
	char *file = cb_ctx;
	size_t rc;
	FILE *fd;

	fd = fopen(file, "w");
	if (!fd) {
		SPDK_ERRLOG("Can't open file %s for write\n", file);
		return -ENOENT;
	}
	rc = fwrite(data, 1, size, fd);
	fclose(fd);

	return rc == size ? 0 : -1;
}

static int
nvmf_ns_reservation_update_json(const struct spdk_nvmf_ns *ns,
				const struct spdk_nvmf_reservation_info *info)
{
	const char *file = ns->ptpl_file;
	struct spdk_json_write_ctx *w;
	uint32_t i;
	int rc = 0;

	w = spdk_json_write_begin(nvmf_ns_json_write_cb, (void *)file, 0);
	if (w == NULL) {
		return -ENOMEM;
	}
	/* clear the configuration file */
	if (!info->ptpl_activated) {
		goto exit;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_bool(w, "ptpl", info->ptpl_activated);
	spdk_json_write_named_uint32(w, "rtype", info->rtype);
	spdk_json_write_named_uint64(w, "crkey", info->crkey);
	spdk_json_write_named_string(w, "bdev_uuid", info->bdev_uuid);
	spdk_json_write_named_string(w, "holder_uuid", info->holder_uuid);

	spdk_json_write_named_array_begin(w, "registrants");
	for (i = 0; i < info->num_regs; i++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint64(w, "rkey", info->registrants[i].rkey);
		spdk_json_write_named_string(w, "host_uuid", info->registrants[i].host_uuid);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

exit:
	rc = spdk_json_write_end(w);
	return rc;
}

static int
nvmf_ns_update_reservation_info(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_reservation_info info;
	struct spdk_nvmf_registrant *reg, *tmp;
	uint32_t i = 0;

	assert(ns != NULL);

	if (!ns->bdev || !nvmf_ns_is_ptpl_capable(ns)) {
		return 0;
	}

	memset(&info, 0, sizeof(info));
	spdk_uuid_fmt_lower(info.bdev_uuid, sizeof(info.bdev_uuid), spdk_bdev_get_uuid(ns->bdev));

	if (ns->rtype) {
		info.rtype = ns->rtype;
		info.crkey = ns->crkey;
		if (!nvmf_ns_reservation_all_registrants_type(ns)) {
			assert(ns->holder != NULL);
			spdk_uuid_fmt_lower(info.holder_uuid, sizeof(info.holder_uuid), &ns->holder->hostid);
		}
	}

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		if (i < SPDK_NVMF_MAX_NUM_REGISTRANTS) {
			spdk_uuid_fmt_lower(info.registrants[i].host_uuid, sizeof(info.registrants[i].host_uuid),
					    &reg->hostid);
			info.registrants[i++].rkey = reg->rkey;
		} else {
			SPDK_ERRLOG("More registrants that can fit into reservation info, truncating\n");
			/* This should never happen as we enforce SPDK_NVMF_MAX_NUM_REGISTRANTS
			 * on ns->registrants. We don't want to continue with missing registrants
			 * from the ptpl state.
			 */
			abort();
		}
	}

	info.num_regs = i;
	info.ptpl_activated = ns->ptpl_activated;

	return nvmf_ns_reservation_update(ns, &info);
}

size_t
nvmf_ns_registrants_get_count(const struct spdk_nvmf_ns *ns)
{
	size_t count = 0;
	struct spdk_nvmf_registrant *reg;

	TAILQ_FOREACH(reg, &ns->registrants, link) {
		count++;
	}
	return count;
}

static struct spdk_nvmf_registrant *
nvmf_ns_reservation_get_registrant(struct spdk_nvmf_ns *ns,
				   struct spdk_uuid *uuid)
{
	struct spdk_nvmf_registrant *reg, *tmp;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		if (!spdk_uuid_compare(&reg->hostid, uuid)) {
			return reg;
		}
	}

	return NULL;
}

/* Generate reservation notice log to registered HostID controllers */
static void
nvmf_subsystem_gen_ctrlr_notification(struct spdk_nvmf_subsystem *subsystem,
				      struct spdk_nvmf_ns *ns,
				      struct spdk_uuid *hostid_list,
				      uint32_t num_hostid,
				      enum spdk_nvme_reservation_notification_log_page_type type)
{
	struct spdk_nvmf_ctrlr *ctrlr;
	uint32_t i;

	for (i = 0; i < num_hostid; i++) {
		TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
			if (!spdk_uuid_compare(&ctrlr->hostid, &hostid_list[i])) {
				nvmf_ctrlr_reservation_notice_log(ctrlr, ns, type);
			}
		}
	}
}

/* Get all registrants' hostid other than the controller who issued the command */
static uint32_t
nvmf_ns_reservation_get_all_other_hostid(struct spdk_nvmf_ns *ns,
		struct spdk_uuid *hostid_list,
		uint32_t max_num_hostid,
		struct spdk_uuid *current_hostid)
{
	struct spdk_nvmf_registrant *reg, *tmp;
	uint32_t num_hostid = 0;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		if (spdk_uuid_compare(&reg->hostid, current_hostid)) {
			if (num_hostid == max_num_hostid) {
				assert(false);
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
static uint32_t
nvmf_ns_reservation_get_unregistered_hostid(struct spdk_uuid *old_hostid_list,
		uint32_t old_num_hostid,
		struct spdk_uuid *remaining_hostid_list,
		uint32_t remaining_num_hostid)
{
	struct spdk_uuid temp_hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t i, j, num_hostid = 0;
	bool found;

	if (!remaining_num_hostid) {
		return old_num_hostid;
	}

	for (i = 0; i < old_num_hostid; i++) {
		found = false;
		for (j = 0; j < remaining_num_hostid; j++) {
			if (!spdk_uuid_compare(&old_hostid_list[i], &remaining_hostid_list[j])) {
				found = true;
				break;
			}
		}
		if (!found) {
			spdk_uuid_copy(&temp_hostid_list[num_hostid++], &old_hostid_list[i]);
		}
	}

	if (num_hostid) {
		memcpy(old_hostid_list, temp_hostid_list, sizeof(struct spdk_uuid) * num_hostid);
	}

	return num_hostid;
}

/* current reservation type is all registrants or not */
static bool
nvmf_ns_reservation_all_registrants_type(struct spdk_nvmf_ns *ns)
{
	return (ns->rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS ||
		ns->rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS);
}

/* current registrant is reservation holder or not */
static bool
nvmf_ns_reservation_registrant_is_holder(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_registrant *reg)
{
	if (!reg) {
		return false;
	}

	if (nvmf_ns_reservation_all_registrants_type(ns)) {
		return true;
	}

	return (ns->holder == reg);
}

static int
nvmf_ns_reservation_add_registrant(struct spdk_nvmf_ns *ns,
				   struct spdk_nvmf_ctrlr *ctrlr,
				   uint64_t nrkey)
{
	struct spdk_nvmf_registrant *reg;

	if (nvmf_ns_registrants_get_count(ns) >= SPDK_NVMF_MAX_NUM_REGISTRANTS) {
		SPDK_ERRLOG("Registrant list full on subsystem: %p, nsid: %u\n", ns->subsystem, ns->nsid);
		return -ENOMEM;
	}

	reg = calloc(1, sizeof(*reg));
	if (!reg) {
		return -ENOMEM;
	}

	reg->rkey = nrkey;
	reg->cntlid = ctrlr->cntlid;
	/* set hostid for the registrant */
	spdk_uuid_copy(&reg->hostid, &ctrlr->hostid);
	TAILQ_INSERT_TAIL(&ns->registrants, reg, link);
	ns->gen++;

	return 0;
}

static void
nvmf_ns_reservation_release_reservation(struct spdk_nvmf_ns *ns)
{
	ns->rtype = 0;
	ns->crkey = 0;
	ns->holder = NULL;
}

/* release the reservation if the last registrant was removed */
static void
nvmf_ns_reservation_check_release_on_remove_registrant(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_registrant *reg)
{
	struct spdk_nvmf_registrant *next_reg;

	/* no reservation holder */
	if (!ns->holder) {
		assert(ns->rtype == 0);
		return;
	}

	next_reg = TAILQ_FIRST(&ns->registrants);
	if (next_reg && nvmf_ns_reservation_all_registrants_type(ns)) {
		/* the next valid registrant is the new holder now */
		ns->holder = next_reg;
	} else if (nvmf_ns_reservation_registrant_is_holder(ns, reg)) {
		/* release the reservation */
		nvmf_ns_reservation_release_reservation(ns);
	}
}

static void
nvmf_ns_reservation_remove_registrant(struct spdk_nvmf_ns *ns,
				      struct spdk_nvmf_registrant *reg)
{
	TAILQ_REMOVE(&ns->registrants, reg, link);
	nvmf_ns_reservation_check_release_on_remove_registrant(ns, reg);
	free(reg);
	ns->gen++;
	return;
}

static uint32_t
nvmf_ns_reservation_remove_registrants_by_key(struct spdk_nvmf_ns *ns,
		uint64_t rkey)
{
	struct spdk_nvmf_registrant *reg, *tmp;
	uint32_t count = 0;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		if (reg->rkey == rkey) {
			nvmf_ns_reservation_remove_registrant(ns, reg);
			count++;
		}
	}
	return count;
}

static void
nvmf_ns_reservation_remove_other_registrants_by_key(struct spdk_nvmf_ns *ns,
		uint64_t rkey, const struct spdk_nvmf_registrant *reg)
{
	struct spdk_nvmf_registrant *reg_tmp, *reg_tmp2;

	TAILQ_FOREACH_SAFE(reg_tmp, &ns->registrants, link, reg_tmp2) {
		if (reg_tmp->rkey == rkey && reg != reg_tmp) {
			nvmf_ns_reservation_remove_registrant(ns, reg_tmp);
		}
	}
}

static uint32_t
nvmf_ns_reservation_remove_all_other_registrants(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_registrant *reg)
{
	struct spdk_nvmf_registrant *reg_tmp, *reg_tmp2;
	uint32_t count = 0;

	TAILQ_FOREACH_SAFE(reg_tmp, &ns->registrants, link, reg_tmp2) {
		if (reg_tmp != reg) {
			nvmf_ns_reservation_remove_registrant(ns, reg_tmp);
			count++;
		}
	}
	return count;
}

static uint32_t
nvmf_ns_reservation_clear_all_registrants(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_registrant *reg, *reg_tmp;
	uint32_t count = 0;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, reg_tmp) {
		nvmf_ns_reservation_remove_registrant(ns, reg);
		count++;
	}
	return count;
}

static void
nvmf_ns_reservation_acquire_reservation(struct spdk_nvmf_ns *ns, uint64_t rkey,
					enum spdk_nvme_reservation_type rtype,
					struct spdk_nvmf_registrant *holder)
{
	ns->rtype = rtype;
	ns->crkey = rkey;
	assert(ns->holder == NULL);
	ns->holder = holder;
}

static bool
nvmf_ns_reservation_register(struct spdk_nvmf_ns *ns,
			     struct spdk_nvmf_ctrlr *ctrlr,
			     struct spdk_nvmf_request *req)
{
	struct spdk_nvme_reservation_register_data key = { 0 };
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint8_t rrega, iekey, cptpl, rtype;
	struct spdk_nvmf_registrant *reg;
	uint8_t status = SPDK_NVME_SC_SUCCESS;
	bool update_sgroup = false;
	struct spdk_uuid hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t num_hostid = 0;
	int rc;

	rrega = cmd->cdw10_bits.resv_register.rrega;
	iekey = cmd->cdw10_bits.resv_register.iekey;
	cptpl = cmd->cdw10_bits.resv_register.cptpl;

	if (req->iovcnt > 0 && req->length >= sizeof(key)) {
		struct spdk_iov_xfer ix;
		spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
		spdk_iov_xfer_to_buf(&ix, &key, sizeof(key));
	} else {
		SPDK_ERRLOG("No key provided. Failing request.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	SPDK_DEBUGLOG(nvmf, "REGISTER: RREGA %u, IEKEY %u, CPTPL %u, "
		      "NRKEY 0x%"PRIx64", NRKEY 0x%"PRIx64"\n",
		      rrega, iekey, cptpl, key.crkey, key.nrkey);

	if (cptpl == SPDK_NVME_RESERVE_PTPL_CLEAR_POWER_ON) {
		/* True to OFF state, and need to be updated in the configuration file */
		if (ns->ptpl_activated) {
			ns->ptpl_activated = 0;
			update_sgroup = true;
		}
	} else if (cptpl == SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS) {
		if (!nvmf_ns_is_ptpl_capable(ns)) {
			status = SPDK_NVME_SC_INVALID_FIELD;
			goto exit;
		} else if (ns->ptpl_activated == 0) {
			ns->ptpl_activated = 1;
			update_sgroup = true;
		}
	}

	/* current Host Identifier has registrant or not */
	reg = nvmf_ns_reservation_get_registrant(ns, &ctrlr->hostid);

	switch (rrega) {
	case SPDK_NVME_RESERVE_REGISTER_KEY:
		if (!reg) {
			/* register new controller */
			if (key.nrkey == 0) {
				SPDK_ERRLOG("Can't register zeroed new key\n");
				status = SPDK_NVME_SC_INVALID_FIELD;
				goto exit;
			}
			rc = nvmf_ns_reservation_add_registrant(ns, ctrlr, key.nrkey);
			if (rc < 0) {
				status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				goto exit;
			}
			update_sgroup = true;
		} else {
			/* register with same key is not an error */
			if (reg->rkey != key.nrkey) {
				SPDK_ERRLOG("The same host already register a "
					    "key with 0x%"PRIx64"\n",
					    reg->rkey);
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				goto exit;
			}
		}
		break;
	case SPDK_NVME_RESERVE_UNREGISTER_KEY:
		if (!reg || (!iekey && reg->rkey != key.crkey)) {
			SPDK_ERRLOG("No registrant or current key doesn't match "
				    "with existing registrant key\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}

		rtype = ns->rtype;
		num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, hostid_list,
				SPDK_NVMF_MAX_NUM_REGISTRANTS,
				&ctrlr->hostid);

		nvmf_ns_reservation_remove_registrant(ns, reg);

		if (!ns->rtype && num_hostid && (rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY ||
						 rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY)) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_RESERVATION_RELEASED);
		}
		update_sgroup = true;
		break;
	case SPDK_NVME_RESERVE_REPLACE_KEY:
		if (key.nrkey == 0) {
			SPDK_ERRLOG("Can't register zeroed new key\n");
			status = SPDK_NVME_SC_INVALID_FIELD;
			goto exit;
		}
		/* Registrant exists */
		if (reg) {
			if (!iekey && reg->rkey != key.crkey) {
				SPDK_ERRLOG("Current key doesn't match "
					    "existing registrant key\n");
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				goto exit;
			}
			if (reg->rkey == key.nrkey) {
				goto exit;
			}
			reg->rkey = key.nrkey;
			if (nvmf_ns_reservation_registrant_is_holder(ns, reg)) {
				ns->crkey = key.nrkey;
			}
		} else if (iekey) { /* No registrant but IEKEY is set */
			/* new registrant */
			rc = nvmf_ns_reservation_add_registrant(ns, ctrlr, key.nrkey);
			if (rc < 0) {
				status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				goto exit;
			}
		} else { /* No registrant */
			SPDK_ERRLOG("No registrant\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;

		}
		update_sgroup = true;
		break;
	default:
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	return update_sgroup;
}

static bool
nvmf_ns_reservation_acquire(struct spdk_nvmf_ns *ns,
			    struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_reservation_acquire_data key = { 0 };
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint8_t racqa, iekey, rtype;
	struct spdk_nvmf_registrant *reg;
	bool all_regs = false;
	uint32_t count = 0;
	bool update_sgroup = true;
	struct spdk_uuid hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t num_hostid = 0;
	struct spdk_uuid new_hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t new_num_hostid = 0;
	bool reservation_released = false;
	bool is_preempt = false;
	bool is_abort = false;
	uint8_t status = SPDK_NVME_SC_SUCCESS;

	racqa = cmd->cdw10_bits.resv_acquire.racqa;
	iekey = cmd->cdw10_bits.resv_acquire.iekey;
	rtype = cmd->cdw10_bits.resv_acquire.rtype;

	if (req->iovcnt > 0 && req->length >= sizeof(key)) {
		struct spdk_iov_xfer ix;
		spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
		spdk_iov_xfer_to_buf(&ix, &key, sizeof(key));
	} else {
		SPDK_ERRLOG("No key provided. Failing request.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

	SPDK_DEBUGLOG(nvmf, "ACQUIRE: RACQA %u, IEKEY %u, RTYPE %u, "
		      "NRKEY 0x%"PRIx64", PRKEY 0x%"PRIx64"\n",
		      racqa, iekey, rtype, key.crkey, key.prkey);

	if (iekey || rtype > SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS) {
		SPDK_ERRLOG("Ignore existing key field set to 1\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

	reg = nvmf_ns_reservation_get_registrant(ns, &ctrlr->hostid);
	/* must be registrant and CRKEY must match */
	if (!reg || reg->rkey != key.crkey) {
		SPDK_ERRLOG("No registrant or current key doesn't match "
			    "with existing registrant key\n");
		status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		update_sgroup = false;
		goto exit;
	}

	all_regs = nvmf_ns_reservation_all_registrants_type(ns);

	switch (racqa) {
	case SPDK_NVME_RESERVE_ACQUIRE:
		/* it's not an error for the holder to acquire same reservation type again */
		if (nvmf_ns_reservation_registrant_is_holder(ns, reg) && ns->rtype == rtype) {
			/* do nothing */
			update_sgroup = false;
		} else if (ns->holder == NULL) {
			/* first time to acquire the reservation */
			nvmf_ns_reservation_acquire_reservation(ns, key.crkey, rtype, reg);
		} else {
			SPDK_ERRLOG("Invalid rtype or current registrant is not holder\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			update_sgroup = false;
			goto exit;
		}
		break;
	case SPDK_NVME_RESERVE_PREEMPT:
	case SPDK_NVME_RESERVE_PREEMPT_ABORT:
		is_preempt = true;
		is_abort = (racqa == SPDK_NVME_RESERVE_PREEMPT_ABORT);

		/* Allocate memory for performing preempt-and-abort on first abort received */
		if (is_abort && !ns->preempt_abort) {
			ns->preempt_abort = calloc(1, sizeof(*ns->preempt_abort));
			if (!ns->preempt_abort) {
				status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				update_sgroup = false;
				goto exit;
			}
		}

		/* Build copy of current other hosts so we can generate a delta
		 * of registrants removed due to the prempt.
		 */
		num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, hostid_list,
				SPDK_NVMF_MAX_NUM_REGISTRANTS,
				&ctrlr->hostid);

		/* no reservation holder */
		if (!ns->holder) {
			/* unregister with PRKEY */
			nvmf_ns_reservation_remove_registrants_by_key(ns, key.prkey);
			break;
		}

		/* only 1 reservation holder and reservation key is valid */
		if (!all_regs) {
			/* preempt itself */
			if (nvmf_ns_reservation_registrant_is_holder(ns, reg) &&
			    ns->crkey == key.prkey) {
				ns->rtype = rtype;
				reservation_released = true;
				break;
			}

			if (ns->crkey == key.prkey) {
				nvmf_ns_reservation_remove_other_registrants_by_key(ns, key.prkey, reg);
				nvmf_ns_reservation_acquire_reservation(ns, key.crkey, rtype, reg);
				reservation_released = true;
			} else if (key.prkey != 0) {
				nvmf_ns_reservation_remove_registrants_by_key(ns, key.prkey);
			} else {
				/* PRKEY is zero */
				SPDK_ERRLOG("Current PRKEY is zero\n");
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				update_sgroup = false;
				goto exit;
			}
		} else {
			/* release all other registrants except for the current one */
			if (key.prkey == 0) {
				nvmf_ns_reservation_remove_all_other_registrants(ns, reg);
				assert(ns->holder == reg);
			} else {
				count = nvmf_ns_reservation_remove_registrants_by_key(ns, key.prkey);
				if (count == 0) {
					SPDK_ERRLOG("PRKEY doesn't match any registrant\n");
					status = SPDK_NVME_SC_RESERVATION_CONFLICT;
					update_sgroup = false;
					goto exit;
				}
			}
		}
		break;
	default:
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		break;
	}

exit:
	if (update_sgroup && is_preempt) {
		new_num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, new_hostid_list,
				 SPDK_NVMF_MAX_NUM_REGISTRANTS,
				 &ctrlr->hostid);
		/* Preempt notification occurs on the unregistered controllers
		 * other than the controller who issued the command.
		 */
		num_hostid = nvmf_ns_reservation_get_unregistered_hostid(hostid_list,
				num_hostid,
				new_hostid_list,
				new_num_hostid);
		if (num_hostid) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_REGISTRATION_PREEMPTED);

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

		}

		/* For Preempt-and-abort copy the hostids for evaluation
		 * of outstanding IO on those controllers on each poll group */
		if (is_abort) {
			struct spdk_nvmf_reservation_preempt_abort_info *p_info = ns->preempt_abort;
			assert(num_hostid <= SPDK_NVMF_MAX_NUM_REGISTRANTS);
			memcpy(p_info->hostids, hostid_list,
			       sizeof(struct spdk_uuid) * num_hostid);
			p_info->hostids_cnt = (uint8_t)num_hostid;
			p_info->hostids_gen++;
			p_info->io_waiting_done = false;
			p_info->io_waiting_timeout_ticks = 0;
		}
	}
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	return update_sgroup;
}

static bool
nvmf_ns_reservation_release(struct spdk_nvmf_ns *ns,
			    struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint8_t rrela, iekey, rtype;
	struct spdk_nvmf_registrant *reg;
	uint64_t crkey = 0;
	uint8_t status = SPDK_NVME_SC_SUCCESS;
	bool update_sgroup = true;
	struct spdk_uuid hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t num_hostid = 0;

	rrela = cmd->cdw10_bits.resv_release.rrela;
	iekey = cmd->cdw10_bits.resv_release.iekey;
	rtype = cmd->cdw10_bits.resv_release.rtype;

	if (req->iovcnt > 0 && req->length >= sizeof(crkey)) {
		struct spdk_iov_xfer ix;
		spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
		spdk_iov_xfer_to_buf(&ix, &crkey, sizeof(crkey));
	} else {
		SPDK_ERRLOG("No key provided. Failing request.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

	SPDK_DEBUGLOG(nvmf, "RELEASE: RRELA %u, IEKEY %u, RTYPE %u, "
		      "CRKEY 0x%"PRIx64"\n",  rrela, iekey, rtype, crkey);

	if (iekey) {
		SPDK_ERRLOG("Ignore existing key field set to 1\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

	reg = nvmf_ns_reservation_get_registrant(ns, &ctrlr->hostid);
	if (!reg || reg->rkey != crkey) {
		SPDK_ERRLOG("No registrant or current key doesn't match "
			    "with existing registrant key\n");
		status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		update_sgroup = false;
		goto exit;
	}

	num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, hostid_list,
			SPDK_NVMF_MAX_NUM_REGISTRANTS,
			&ctrlr->hostid);

	switch (rrela) {
	case SPDK_NVME_RESERVE_RELEASE:
		if (!ns->holder) {
			SPDK_DEBUGLOG(nvmf, "RELEASE: no holder\n");
			update_sgroup = false;
			goto exit;
		}
		if (ns->rtype != rtype) {
			SPDK_ERRLOG("Type doesn't match\n");
			status = SPDK_NVME_SC_INVALID_FIELD;
			update_sgroup = false;
			goto exit;
		}
		if (!nvmf_ns_reservation_registrant_is_holder(ns, reg)) {
			/* not the reservation holder, this isn't an error */
			update_sgroup = false;
			goto exit;
		}

		rtype = ns->rtype;
		nvmf_ns_reservation_release_reservation(ns);

		if (num_hostid && rtype != SPDK_NVME_RESERVE_WRITE_EXCLUSIVE &&
		    rtype != SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_RESERVATION_RELEASED);
		}
		break;
	case SPDK_NVME_RESERVE_CLEAR:
		nvmf_ns_reservation_clear_all_registrants(ns);
		if (num_hostid) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_RESERVATION_PREEMPTED);
		}
		break;
	default:
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	return update_sgroup;
}

static void
nvmf_ns_reservation_report(const struct spdk_nvmf_ns *ns,
			   struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_registrant *reg, *tmp;
	struct spdk_nvme_reservation_status_extended_data status_data = { 0 };
	struct spdk_iov_xfer ix;
	uint32_t transfer_len;
	uint32_t regctl = 0;
	uint8_t status = SPDK_NVME_SC_SUCCESS;

	if (req->iovcnt == 0) {
		SPDK_ERRLOG("No data transfer specified for request. "
			    " Unable to transfer back response.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	if (!cmd->cdw11_bits.resv_report.eds) {
		SPDK_ERRLOG("NVMeoF uses extended controller data structure, "
			    "please set EDS bit in cdw11 and try again\n");
		status = SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT;
		goto exit;
	}

	/* Number of Dwords of the Reservation Status data structure to transfer */
	transfer_len = (cmd->cdw10 + 1) * sizeof(uint32_t);

	if (transfer_len < sizeof(struct spdk_nvme_reservation_status_extended_data)) {
		status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto exit;
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);

	status_data.data.gen = ns->gen;
	status_data.data.rtype = ns->rtype;
	status_data.data.ptpls = ns->ptpl_activated;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		regctl++;
	}

	/*
	 * We report the number of registrants as per the spec here, even if
	 * the iov isn't big enough to contain them all. In that case, the
	 * spdk_iov_xfer_from_buf() won't actually copy any of the remaining
	 * data; as it keeps track of the iov cursor itself, it's simplest to
	 * just walk the entire list anyway.
	 */
	status_data.data.regctl = regctl;

	spdk_iov_xfer_from_buf(&ix, &status_data, sizeof(status_data));

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		struct spdk_nvme_registered_ctrlr_extended_data ctrlr_data = { 0 };

		ctrlr_data.cntlid = reg->cntlid ? reg->cntlid : 0xffff;
		ctrlr_data.rcsts.status = (ns->holder == reg) ? true : false;
		ctrlr_data.rkey = reg->rkey;
		spdk_uuid_copy((struct spdk_uuid *)ctrlr_data.hostid, &reg->hostid);

		spdk_iov_xfer_from_buf(&ix, &ctrlr_data, sizeof(ctrlr_data));
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	return;
}

static void
nvmf_ns_reservation_complete(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;

	spdk_nvmf_request_complete(req);
}

static void
ns_reservation_pg_io_wait_check(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	struct spdk_nvmf_subsystem_pg_ns_info *pg_ns;

	ns = spdk_io_channel_iter_get_ctx(i);
	group = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));
	sgroup = &group->sgroups[ns->subsystem->id];
	pg_ns = &sgroup->ns_info[ns->nsid - 1];

	/* Pass io_waiting count as result, this will provide the following:
	 *	1) If non-zero, this will immedately end the channel walk
	 *	2) If zero, this will continue to next pg to check their io_waiting.
	 *	3) If last pg reports 0, all IO waiting is done and completion is
	 *	called with 0
	 */
	spdk_for_each_channel_continue(i, pg_ns->preempt_abort.io_waiting);
}

static void ns_reservation_sched_next_io_wait_check(struct spdk_nvmf_ns *ns);

static void
ns_reservation_pg_io_wait_check_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_ns *ns = spdk_io_channel_iter_get_ctx(i);

	if (!status) {
		SPDK_DEBUGLOG(nvmf, "subsystem: %p, nsid: %u done waiting on IOs\n",
			      ns->subsystem, ns->nsid);
		ns->preempt_abort->io_waiting_done = true;
		_nvmf_ns_reservation_update_done(ns->subsystem,
						 STAILQ_FIRST(&ns->reservations), 0);
	} else {
		SPDK_DEBUGLOG(nvmf, "subsystem: %p, nsid: %u still waiting on %i IOs\n",
			      ns->subsystem, ns->nsid, status);
		ns_reservation_sched_next_io_wait_check(ns);
	}
}

static void
ns_reservation_pg_io_wait_clear_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_ns *ns = (struct spdk_nvmf_ns *)spdk_io_channel_iter_get_ctx(i);
	/* If we entered this function we are always timed out */
	_nvmf_ns_reservation_update_done(ns->subsystem,
					 STAILQ_FIRST(&ns->reservations), -ETIMEDOUT);
}

static void
ns_reservation_pg_io_wait_clear(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_ns *ns = (struct spdk_nvmf_ns *)spdk_io_channel_iter_get_ctx(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));
	struct spdk_nvmf_request *q_req;
	struct spdk_nvmf_qpair *qpair;
	struct spdk_nvmf_reservation_preempt_abort_info *p_info = ns->preempt_abort;
	bool hostid_match;

	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		if (!qpair->ctrlr || qpair->ctrlr->subsys != ns->subsystem) {
			continue;
		}
		hostid_match = ns_reservation_hostid_list_contains_id(p_info->hostids,
				p_info->hostids_cnt, &qpair->ctrlr->hostid);
		if (!hostid_match) {
			continue;
		}
		TAILQ_FOREACH(q_req, &qpair->outstanding, link) {
			struct spdk_nvme_cmd *req_cmd = &q_req->cmd->nvme_cmd;
			if (req_cmd->nsid == ns->nsid && q_req->reservation_waiting) {
				q_req->reservation_waiting = 0;
			}
		}
	}
	spdk_for_each_channel_continue(i, 0);
}

static int
ns_reservation_next_io_wait_check(void *ctx)
{
	struct spdk_nvmf_ns *ns = (struct spdk_nvmf_ns *)ctx;
	struct spdk_nvmf_reservation_preempt_abort_info *p_info = ns->preempt_abort;

	/* this should not be running if io_waiting is complete */
	assert(!p_info->io_waiting_done);

	if (spdk_get_ticks() < p_info->io_waiting_timeout_ticks) {
		/* Start a poll group check */
		spdk_for_each_channel(ns->subsystem->tgt,
				      ns_reservation_pg_io_wait_check,
				      ns,
				      ns_reservation_pg_io_wait_check_done);
	} else {
		/* If the cmd timed out we call update_done during cleanup */
		spdk_for_each_channel(ns->subsystem->tgt,
				      ns_reservation_pg_io_wait_clear,
				      ns,
				      ns_reservation_pg_io_wait_clear_done);
	}

	spdk_poller_unregister(&p_info->io_waiting_timer);
	return SPDK_POLLER_BUSY;
}

#define NS_RESERVATION_IO_WAIT_CHECK_INTERVAL 100
#define NS_RESERVATION_IO_WAIT_TIMEOUT_S 10
static void
ns_reservation_sched_next_io_wait_check(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_reservation_preempt_abort_info *p_info = ns->preempt_abort;
	assert(p_info);
	assert(p_info->io_waiting_timer == NULL);

	/* First time scheduling, calculate a total timeout */
	if (!p_info->io_waiting_timeout_ticks) {
		p_info->io_waiting_timeout_ticks = spdk_get_ticks() +
						   NS_RESERVATION_IO_WAIT_TIMEOUT_S * spdk_get_ticks_hz();
	}
	/* We use a poller as a one-shot timer for next check */
	p_info->io_waiting_timer =
		SPDK_POLLER_REGISTER(ns_reservation_next_io_wait_check, ns, NS_RESERVATION_IO_WAIT_CHECK_INTERVAL);
}

static void
_nvmf_ns_reservation_update_done(struct spdk_nvmf_subsystem *subsystem,
				 void *cb_arg, int status)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)cb_arg;
	struct spdk_nvmf_poll_group *group = req->qpair->group;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_ns *ns;

	assert(subsystem->thread == spdk_get_thread());

	if (status != 0) {
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
	assert(ns != NULL);

	/* sanity check: this req should be head of outstanding */
	assert(req->reservation_queued == true);
	assert(req == STAILQ_FIRST(&ns->reservations));

	if (!status && ns_reservation_req_is_preempt_abort(req) && !ns->preempt_abort->io_waiting_done) {
		/* Check for io_waiting completion */
		spdk_for_each_channel(ns->subsystem->tgt,
				      ns_reservation_pg_io_wait_check,
				      ns,
				      ns_reservation_pg_io_wait_check_done);
		return;
	}

	/* req is complete, remove from queue and continue if there's others */
	STAILQ_REMOVE_HEAD(&ns->reservations, reservation_link);
	req->reservation_queued = false;
	if (!STAILQ_EMPTY(&ns->reservations)) {
		/* NOTE: we leave the next on the queue to prevent any in-flight
		 * requests moving from pg->thread to subsystem->thread from
		 * executing before the next one
		 */
		spdk_thread_send_msg(subsystem->thread, nvmf_ns_reservation_request,
				     STAILQ_FIRST(&ns->reservations));
	}

	/* Complete the request on the original pg */
	spdk_thread_send_msg(group->thread, nvmf_ns_reservation_complete, req);
}

static void
nvmf_ns_reservation_update_state(struct spdk_nvmf_ns *ns,
				 struct spdk_nvmf_ctrlr *ctrlr,
				 struct spdk_nvmf_request *req,
				 enum spdk_nvme_nvm_opcode opc)
{
	bool update_sgroup = false;
	int status = 0;

	/* All reservation state modifications must be queued to serialize them */
	if (!req->reservation_queued) {
		STAILQ_INSERT_TAIL(&ns->reservations, req, reservation_link);
		req->reservation_queued = true;
	}
	/* The head is in-progress, others must wait */
	if (req != STAILQ_FIRST(&ns->reservations)) {
		return;
	}

	switch (opc) {
	case SPDK_NVME_OPC_RESERVATION_REGISTER:
		update_sgroup = nvmf_ns_reservation_register(ns, ctrlr, req);
		break;
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		update_sgroup = nvmf_ns_reservation_acquire(ns, ctrlr, req);
		break;
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		update_sgroup = nvmf_ns_reservation_release(ns, ctrlr, req);
		break;
	default:
		break;
	}

	/* update reservation information to subsystem's poll group */
	if (update_sgroup) {
		if (ns->ptpl_activated || opc == SPDK_NVME_OPC_RESERVATION_REGISTER) {
			if (nvmf_ns_update_reservation_info(ns) != 0) {
				req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			}
		}
		spdk_for_each_channel(ns->subsystem->tgt,
				      ns_reservation_pg_update,
				      ns,
				      ns_reservation_pg_update_done);
		return;
	}

	_nvmf_ns_reservation_update_done(ctrlr->subsys, req, status);
}

void
nvmf_ns_reservation_request(void *ctx)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)ctx;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	uint32_t nsid;
	struct spdk_nvmf_ns *ns;

	nsid = cmd->nsid;
	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, nsid);
	assert(ns != NULL);

	/* Report is a read-only command and can always be executed */
	if (cmd->opc == SPDK_NVME_OPC_RESERVATION_REPORT) {
		nvmf_ns_reservation_report(ns, req);
		/* Complete the request on the original pg */
		spdk_thread_send_msg(req->qpair->group->thread, nvmf_ns_reservation_complete, req);
	} else {
		/* Remaining commands modify reservation state and must be serialized.
		 * These complete asynchronously after state propagates to poll groups
		 */
		nvmf_ns_reservation_update_state(ns, ctrlr, req, cmd->opc);
	}
}

static bool
nvmf_ns_is_ptpl_capable_json(const struct spdk_nvmf_ns *ns)
{
	return ns->ptpl_file != NULL;
}

static struct spdk_nvmf_ns_reservation_ops g_reservation_ops = {
	.is_ptpl_capable = nvmf_ns_is_ptpl_capable_json,
	.update = nvmf_ns_reservation_update_json,
	.load = nvmf_ns_reservation_load_json,
};

bool
nvmf_ns_is_ptpl_capable(const struct spdk_nvmf_ns *ns)
{
	return g_reservation_ops.is_ptpl_capable(ns);
}

struct spdk_nvme_rescap
nvmf_ns_get_rescap(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvme_rescap rescap = {
		.ptpls = nvmf_ns_is_ptpl_capable(ns),
		.wes = 1,
		.eas = 1,
		.weros = 1,
		.earos = 1,
		.wears = 1,
		.eaars = 1,
		.ieks = 1,
	};
	return rescap;
}

static int
nvmf_ns_reservation_update(const struct spdk_nvmf_ns *ns,
			   const struct spdk_nvmf_reservation_info *info)
{
	return g_reservation_ops.update(ns, info);
}

static int
nvmf_ns_reservation_load(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info)
{
	return g_reservation_ops.load(ns, info);
}

void
spdk_nvmf_set_custom_ns_reservation_ops(const struct spdk_nvmf_ns_reservation_ops *ops)
{
	g_reservation_ops = *ops;
}

int
spdk_nvmf_subsystem_set_ana_reporting(struct spdk_nvmf_subsystem *subsystem,
				      bool ana_reporting)
{
	if (subsystem->state != SPDK_NVMF_SUBSYSTEM_INACTIVE) {
		return -EAGAIN;
	}

	subsystem->flags.ana_reporting = ana_reporting;

	return 0;
}

bool
spdk_nvmf_subsystem_get_ana_reporting(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->flags.ana_reporting;
}

struct subsystem_listener_update_ctx {
	struct spdk_nvmf_subsystem_listener *listener;

	spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn;
	void *cb_arg;
};

static void
subsystem_listener_update_done(struct spdk_io_channel_iter *i, int status)
{
	struct subsystem_listener_update_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, status);
	}
	free(ctx);
}

static void
subsystem_listener_update_on_pg(struct spdk_io_channel_iter *i)
{
	struct subsystem_listener_update_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_nvmf_subsystem_listener *listener;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_ctrlr *ctrlr;

	listener = ctx->listener;
	group = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));

	TAILQ_FOREACH(ctrlr, &listener->subsystem->ctrlrs, link) {
		if (ctrlr->thread != spdk_get_thread()) {
			continue;
		}

		if (ctrlr->admin_qpair && ctrlr->admin_qpair->group == group && ctrlr->listener == listener) {
			nvmf_ctrlr_async_event_ana_change_notice(ctrlr);
		}
	}

	spdk_for_each_channel_continue(i, 0);
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
	assert(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	       subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED);

	if (!subsystem->flags.ana_reporting) {
		SPDK_ERRLOG("ANA reporting is disabled\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	/* ANA Change state is not used, ANA Persistent Loss state
	 * is not supported yet.
	 */
	if (!(ana_state == SPDK_NVME_ANA_OPTIMIZED_STATE ||
	      ana_state == SPDK_NVME_ANA_NON_OPTIMIZED_STATE ||
	      ana_state == SPDK_NVME_ANA_INACCESSIBLE_STATE)) {
		SPDK_ERRLOG("ANA state %d is not supported\n", ana_state);
		cb_fn(cb_arg, -ENOTSUP);
		return;
	}

	if (anagrpid > subsystem->max_nsid) {
		SPDK_ERRLOG("ANA group ID %" PRIu32 " is more than maximum\n", anagrpid);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	listener = nvmf_subsystem_find_listener(subsystem, trid);
	if (!listener) {
		SPDK_ERRLOG("Unable to find listener.\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (anagrpid != 0 && listener->ana_state[anagrpid - 1] == ana_state) {
		cb_fn(cb_arg, 0);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Unable to allocate context\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	for (i = 1; i <= subsystem->max_nsid; i++) {
		if (anagrpid == 0 || i == anagrpid) {
			listener->ana_state[i - 1] = ana_state;
		}
	}
	listener->ana_state_change_count++;

	ctx->listener = listener;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(subsystem->tgt,
			      subsystem_listener_update_on_pg,
			      ctx,
			      subsystem_listener_update_done);
}

int
spdk_nvmf_subsystem_get_ana_state(struct spdk_nvmf_subsystem *subsystem,
				  const struct spdk_nvme_transport_id *trid,
				  uint32_t anagrpid,
				  enum spdk_nvme_ana_state *ana_state)
{
	assert(ana_state != NULL);

	struct spdk_nvmf_subsystem_listener *listener;

	if (!subsystem->flags.ana_reporting) {
		SPDK_ERRLOG("ANA reporting is disabled\n");
		return -EINVAL;
	}

	if (anagrpid <= 0 || anagrpid > subsystem->max_nsid) {
		SPDK_ERRLOG("ANA group ID %" PRIu32 " is invalid\n", anagrpid);
		return -EINVAL;
	}

	listener = nvmf_subsystem_find_listener(subsystem, trid);
	if (!listener) {
		SPDK_ERRLOG("Unable to find listener.\n");
		return -EINVAL;
	}

	*ana_state = listener->ana_state[anagrpid - 1];
	return 0;
}

bool
spdk_nvmf_subsystem_is_discovery(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT ||
	       subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY;
}

bool
nvmf_nqn_is_discovery(const char *nqn)
{
	return strcmp(nqn, SPDK_NVMF_DISCOVERY_NQN) == 0;
}
