/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2025 Nutanix Inc.
 */

/*
 * [한국어 설명] NVMe 공용 유틸리티 - Transport ID 파싱 / usage 출력 / controller 이름 빌더 (nvme_util.c)
 *
 * === 파일의 역할 ===
 * SPDK NVMe 드라이버를 사용하는 다양한 애플리케이션(perf, identify, fio nvme 엔진,
 * bdev_nvme 등)이 **공통적으로 필요로 하는 헬퍼 3종**을 제공한다:
 *   1) `spdk_nvme_transport_id_usage()` - CLI에서 -r/--transport 옵션 사용법(help 메시지)을
 *      옵션 비트마스크에 따라 동적으로 출력. PCIe/Fabric 활성화 여부, namespace/hostnqn/
 *      alt_traddr 키 노출 여부를 toggle.
 *   2) `spdk_nvme_trid_entry_parse()` - "trtype:PCIe traddr:0000:04:00.0 ns:1 hostnqn:..." 같은
 *      key:value 문자열을 `struct spdk_nvme_trid_entry`(trid + nsid + hostnqn + failover_trid)로
 *      파싱. CLI 입력의 표준 진입점.
 *   3) `spdk_nvme_build_name()` - controller + (선택적) namespace를 사람이 읽을 수 있는
 *      이름 문자열("PCIE (0000:04:00.0) [8086:0a54] NSID 1")로 직렬화. 로그/통계 표기용.
 *
 * 이들은 NVMe 드라이버 코어 동작(qpair/CQE/PRP)과 무관한 "주변 유틸"이지만, NVMe API를
 * 사용하는 거의 모든 SPDK 앱이 import하므로 lib/nvme에 공통으로 두고 spdk_internal/nvme_util.h를
 * 통해 노출한다.
 *
 * **본 파일은 코드 수정 없이 한국어 주석만 추가/보강된 학습 사본**이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK NVMe 애플리케이션의 전형적인 시작 흐름:
 *   main() → getopt 등으로 -r 옵션 파싱
 *           → spdk_nvme_trid_entry_parse(entry, optarg)  ← [이 파일]
 *           → spdk_nvme_probe(&entry->trid, ..., probe_cb, attach_cb, ...)
 *             → attach_cb에서 spdk_nvme_build_name(name, len, ctrlr, ns)  ← [이 파일]
 *               → 이 이름을 SPDK_NOTICELOG/통계 라벨로 사용.
 *   --help 출력 시:
 *           → spdk_nvme_transport_id_usage(stdout, opts)  ← [이 파일]
 *
 * 호출 체인:
 *   - usage 경로:  app::usage() → spdk_nvme_transport_id_usage → fprintf
 *   - parse 경로:  app::parse_args() → spdk_nvme_trid_entry_parse →
 *                  spdk_nvme_transport_id_parse(nvme_transport.c) + strcasestr/spdk_strtol
 *   - build 경로:  attach_cb → spdk_nvme_build_name →
 *                  spdk_nvme_ctrlr_get_transport_id, spdk_nvme_ctrlr_get_pci_device,
 *                  spdk_pci_device_get_id, spdk_nvme_ns_get_id
 *
 * 실행 컨텍스트: 모두 **앱 init/setup 단계의 메인 스레드**에서 호출되며, I/O hot path가
 * 아니므로 성능 민감하지 않다 (snprintf/strcasestr 사용). reactor 외부에서도 호출 가능.
 *
 * === 타 모듈과의 연결 ===
 *  - **spdk_internal/nvme_util.h** — 본 파일의 함수 프로토타입 + struct spdk_nvme_trid_entry
 *    (trid + nsid + hostnqn[] + failover_trid) + USAGE_OPT_* 비트 정의.
 *  - **spdk/nvme.h** — `struct spdk_nvme_transport_id`, `enum spdk_nvme_transport_type`(PCIe/RDMA/
 *    TCP/VFIOUSER/CUSTOM), `spdk_nvme_transport_id_parse`(같은 헤더, nvme_transport.c 정의),
 *    `spdk_nvme_ctrlr_get_transport_id`, `spdk_nvme_ctrlr_get_pci_device`, `spdk_nvme_ns_get_id`.
 *  - **spdk/nvmf_spec.h** — `SPDK_NVMF_DISCOVERY_NQN`(default subnqn), `SPDK_NVMF_TRADDR_MAX_LEN`.
 *  - **spdk/log.h** — `SPDK_ERRLOG` (파싱 에러 보고).
 *  - **spdk/string.h** — `spdk_strtol`(범위 안전한 strtol 래퍼, 음수/오버플로 검증).
 *  - **spdk/env.h (PCI)** — `struct spdk_pci_device`, `struct spdk_pci_id`,
 *    `spdk_pci_device_get_id` (PCIe vendor/device id 추출).
 *  - **호출자** — perf/identify/nvme_manage 등 SPDK 표준 NVMe 애플리케이션 + bdev_nvme/nvmf
 *    타깃의 옵션 파싱 코드.
 *
 * 데이터 흐름:
 *   사용자 CLI 문자열 → spdk_nvme_trid_entry_parse → spdk_nvme_trid_entry →
 *     spdk_nvme_probe 호출 → controller attach → spdk_nvme_build_name → 사람이 읽는 이름
 *
 * === 주요 함수/구조체 요약 ===
 *  핵심 자료구조 (정의는 spdk_internal/nvme_util.h):
 *    - struct spdk_nvme_trid_entry: { trid(spdk_nvme_transport_id), nsid(uint16_t),
 *      hostnqn[NQN_MAX_LEN+1], failover_trid(spdk_nvme_transport_id) } - CLI 한 줄을 담는 구조체.
 *    - SPDK_NVME_TRID_USAGE_OPT_* 플래그: MANDATORY(필수 표시), NO_PCIE(PCIe 숨김),
 *      NO_FABRIC(Fabric 숨김), LONGOPT(--transport 표기), NS(ns 키 노출),
 *      HOSTNQN(hostnqn 키 노출), ALT_TRADDR(alt_traddr 노출), MULTI(다중 지정 가능 노트).
 *
 *  주요 함수:
 *    - spdk_nvme_transport_id_usage — --help 출력. 옵션 비트마스크에 따라 동적 출력.
 *    - spdk_nvme_trid_entry_parse — CLI key:value 문자열 → trid_entry 구조체 변환.
 *      ns, hostnqn, alt_traddr는 표준 trid 파서가 모르므로 strcasestr로 직접 추출.
 *    - spdk_nvme_build_name — ctrlr+ns → 사람-친화 라벨 (트랜스포트별 포맷팅).
 */

#include "spdk_internal/nvme_util.h"
/* [한국어] 본 파일이 노출하는 함수의 프로토타입 + 관련 구조체/플래그 정의 헤더.
 * spdk_internal/은 SPDK 내부 모듈끼리 공유하지만 외부 사용자에겐 안정성 보장 안 하는
 * 헤더 모음. spdk_nvme_trid_entry, SPDK_NVME_TRID_USAGE_OPT_* 등을 가져온다. */

#include "spdk/log.h"
/* [한국어] SPDK 통합 로그 매크로 - SPDK_ERRLOG/NOTICELOG/INFOLOG. 파싱 에러 보고용. */

#include "spdk/string.h"
/* [한국어] SPDK의 안전한 문자열 유틸 - 본 파일에서는 spdk_strtol(범위 검증된 strtol 래퍼).
 * 표준 strtol은 errno 처리가 까다로워 SPDK는 자체 안전 래퍼 제공. */

/*
 * [한국어]
 * spdk_nvme_transport_id_usage - --help 화면에 -r/--transport 옵션 사용법을 옵션 마스크 따라 출력
 *
 * @f:    출력 대상 FILE* (보통 stdout, 에러 시 stderr).
 * @opts: 표시할 항목 비트마스크 - SPDK_NVME_TRID_USAGE_OPT_* 조합:
 *        - MANDATORY: 필수 옵션으로 표기 (괄호 없음, 기본은 [optional]).
 *        - NO_PCIE / NO_FABRIC: 해당 트랜스포트 숨김 (둘 다 끄면 거의 무의미).
 *        - LONGOPT: ", --transport" 표기 추가.
 *        - NS / HOSTNQN / ALT_TRADDR: 추가 키 노출.
 *        - MULTI: "여러 번 지정 가능" 노트 추가.
 *
 * @return: 없음 (void). fprintf 실패는 무시 (사용성 메시지이므로 best-effort).
 *
 * 왜 필요한가: SPDK 표준 NVMe 앱(perf, identify, nvme_manage 등)이 동일한 -r 옵션 문법을
 *   사용하므로 사용법 문자열을 한 곳에서 관리. 앱별로 PCIe만 / Fabric만 / 양쪽 / ns 옵션 등의
 *   세트가 달라 비트마스크로 토글.
 *
 * 동작 단계:
 *   1) opts에서 4개 boolean 플래그 추출: mandatory/pcie/fabric/both.
 *   2) PCIe 예시 주소("0000:04:00.0"), Fabric 예시 주소("192.168.100.8") 결정.
 *   3) "or" 연결자: 양쪽 모두 표시할 때만 " or " 사용.
 *   4) 첫 줄: -r/--transport 헤더 라인 (mandatory면 [] 없이, 아니면 [] 포함).
 *   5) Format/Keys 섹션 - trtype 항상, adrfam은 fabric 시.
 *   6) traddr 라인 - PCIe/fabric 예시 동시 또는 단독.
 *   7) trsvcid/subnqn은 fabric 시.
 *   8) ns/hostnqn/alt_traddr는 각 비트 활성 시.
 *   9) Examples 섹션 + MULTI 노트.
 *
 * 실행 컨텍스트: 앱 init/usage 출력 단계의 메인 스레드. I/O 경로 아님.
 *
 * caller: 각 SPDK NVMe 앱의 usage()/print_help() 함수.
 * callee: fprintf만 사용 - 외부 의존 최소.
 *
 * 호출 체인:
 *   app::usage() → [spdk_nvme_transport_id_usage] → fprintf
 */
void
spdk_nvme_transport_id_usage(FILE *f, uint32_t opts)
{
	bool mandatory = opts & SPDK_NVME_TRID_USAGE_OPT_MANDATORY;
	/* [한국어] -r 옵션이 필수인지 여부 - true면 헤더에 [] 없이 표시. */

	bool pcie = !(opts & SPDK_NVME_TRID_USAGE_OPT_NO_PCIE);
	/* [한국어] PCIe 트랜스포트 표시 여부 - NO_PCIE 비트가 *없으면* 표시 (default ON).
	 * SPDK 앱 대부분이 PCIe 지원하므로 default ON, opt-out 디자인. */

	bool fabric = !(opts & SPDK_NVME_TRID_USAGE_OPT_NO_FABRIC);
	/* [한국어] Fabric(RDMA/TCP/FC) 트랜스포트 표시 여부 - NO_FABRIC 없으면 표시 (default ON). */

	bool both = pcie && fabric;
	/* [한국어] 양쪽 모두 표시 - "or" 연결자, Examples 두 가지 표시 등에 사용. */

	const char *pcie_addr = pcie ? "0000:04:00.0" : "";
	/* [한국어] PCIe 예시 주소 - PCI BDF 표기 (Bus:Device.Function), 아무 예시. */

	const char *fabric_addr = fabric ? "192.168.100.8" : "";
	/* [한국어] Fabric 예시 IP - RDMA/TCP target IP 자리. */

	const char * or = both ? " or " : "";
	/* [한국어] 양쪽 예시를 한 줄에 보일 때 연결자. " or " 또는 빈 문자열.
	 * 변수명 'or'는 C에서 키워드 아님 - 단순 식별자. */

	fprintf(f, "\t%s-r%s <fmt> Transport ID for %s%s%s%s\n", mandatory ? "" : "[",
		opts & SPDK_NVME_TRID_USAGE_OPT_LONGOPT ? ", --transport" : "", pcie ? "local PCIe NVMe" : "", or
		, fabric ? "NVMeoF" : "", mandatory ? "" : "]");
	/* [한국어] 메인 헤더 라인 출력. 형태 예시:
	 *   "\t[-r, --transport <fmt> Transport ID for local PCIe NVMe or NVMeoF]"
	 * mandatory면 [/] 양쪽 빈 문자열, LONGOPT면 ", --transport" 추가. */

	fprintf(f, "\t\tFormat: 'key:value [key:value] ...'\n");
	/* [한국어] CLI 입력 형식 안내 - 공백 구분 key:value 페어. */

	fprintf(f, "\t\tKeys:\n");
	/* [한국어] 사용 가능한 key 리스트 헤더. */

	fprintf(f, "\t\t trtype      Transport type (e.g. PCIe, RDMA)\n");
	/* [한국어] trtype은 모든 경우 필수 - PCIe/RDMA/TCP/FC/VFIOUSER/CUSTOM 중 하나. */

	if (fabric) {
		/* [한국어] adrfam은 Fabric에서만 의미 (PCIe는 BDF로 충분). */
		fprintf(f, "\t\t adrfam      Address family (e.g. IPv4, IPv6)\n");
	}

	fprintf(f, "\t\t traddr      Transport address (e.g. %s%s%s)\n", pcie_addr, or, fabric_addr);
	/* [한국어] traddr 라인 - PCIe면 BDF, Fabric이면 IP. 양쪽이면 "BDF or IP" 형식. */

	if (fabric) {
		fprintf(f, "\t\t trsvcid     Transport service identifier (e.g. 4420)\n");
		/* [한국어] Fabric 전용 - TCP/RDMA 포트 번호 (4420은 NVMe-oF 표준 포트). */

		fprintf(f, "\t\t subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
		/* [한국어] Subsystem NVMe Qualified Name - 미지정 시 NVMe-oF 디스커버리 controller로 연결. */
	}

	if (opts & SPDK_NVME_TRID_USAGE_OPT_NS) {
		/* [한국어] 일부 앱(perf 등)은 namespace 단위 동작 → ns 키 노출. */
		fprintf(f, "\t\t %-11s NVMe namespace ID (all active namespaces are used by default)\n", "ns");
	}

	if (fabric && opts & SPDK_NVME_TRID_USAGE_OPT_HOSTNQN) {
		/* [한국어] Fabric에서 host 측을 식별하는 NQN - 보안/격리에 사용.
		 * PCIe에서는 의미 없음. */
		fprintf(f, "\t\t %-11s Host NQN\n", "hostnqn");
	}

	if (fabric && opts & SPDK_NVME_TRID_USAGE_OPT_ALT_TRADDR) {
		/* [한국어] failover 대체 주소 - primary 다운 시 alt로 자동 전환. multipath 시나리오. */
		fprintf(f, "\t\t %-11s Alternative Transport address for failover (optional)\n", "alt_traddr");
	}

	fprintf(f, "\t\tExamples:\n");
	/* [한국어] 실제 사용 예시 섹션 헤더. */

	if (both || pcie) {
		/* [한국어] PCIe 예시는 PCIe가 활성화된 경우 출력. */
		fprintf(f, "\t\t -r 'trtype:PCIe traddr:%s'\n", pcie_addr);
	}

	if (both || fabric) {
		/* [한국어] Fabric 예시는 Fabric 활성 시 - RDMA + IPv4 + 표준 포트 4420. */
		fprintf(f, "\t\t -r 'trtype:RDMA adrfam:IPv4 traddr:%s trsvcid:4420'\n", fabric_addr);
	}

	if (opts & SPDK_NVME_TRID_USAGE_OPT_MULTI) {
		/* [한국어] perf 등은 -r를 여러 번 받아 다중 디스크 동시 테스트 가능 - 그 안내. */
		fprintf(f, "\t\tNote: can be specified multiple times to test multiple disks/targets.\n");
	}
}

/*
 * [한국어]
 * spdk_nvme_trid_entry_parse - "key:value [key:value]..." CLI 문자열을 trid_entry로 파싱
 *
 * @trid_entry: 결과를 저장할 출력 구조체. trid + nsid + hostnqn + failover_trid 보유.
 *              호출 전 zeroed 권장 (특히 hostnqn/nsid/failover_trid).
 * @str:        사용자가 -r로 넘긴 원본 문자열 (예: "trtype:PCIe traddr:0000:04:00.0 ns:1").
 *
 * @return: 0 = 파싱 성공, -EINVAL = trtype/traddr/etc 형식 오류 또는 nsid 범위 초과 등.
 *
 * 왜 필요한가: SPDK 표준 trid 파서(`spdk_nvme_transport_id_parse`)는 NVMe 표준 키
 *   (trtype/adrfam/traddr/trsvcid/subnqn)만 알기 때문에, 비표준 확장 키
 *   (ns, hostnqn, alt_traddr)는 이 함수에서 직접 추출해야 한다.
 *
 * 동작 단계:
 *   1) 기본값 설정 - trtype = PCIe(가장 흔함), subnqn = DISCOVERY_NQN(미지정 시 기본).
 *   2) 표준 파서 호출 - trtype/adrfam/traddr/trsvcid/subnqn을 trid에 채움.
 *   3) "ns:" 또는 "ns=" 추출 - 5자리 이내 정수, 1~65535 범위 검증 → trid_entry->nsid.
 *   4) "hostnqn:" 또는 "hostnqn=" 추출 - 길이 검증 → trid_entry->hostnqn 복사.
 *   5) failover_trid를 primary trid로 초기화 (alt_traddr 없으면 동일).
 *   6) "alt_traddr:" 또는 "alt_traddr=" 추출 - 길이 검증 → failover_trid.traddr 덮어쓰기.
 *
 * 실행 컨텍스트: 앱 init 단계의 메인 스레드 - I/O hot path 아님. strcasestr/snprintf 사용.
 *
 * 주의: 키 구분자로 ':' 또는 '='를 모두 허용 (case-insensitive 검색). 같은 키가 여러 번
 *   나오면 첫 번째만 사용 (strcasestr 첫 매치).
 *
 * caller: 각 NVMe 앱의 옵션 파싱 코드 (perf/identify/bdev_nvme_rpc 등).
 * callee: spdk_nvme_transport_id_parse(nvme_transport.c), strcasestr, strcspn, memcpy,
 *   spdk_strtol, snprintf, SPDK_ERRLOG.
 *
 * 호출 체인:
 *   app::parse_args → [spdk_nvme_trid_entry_parse]
 *     → spdk_nvme_transport_id_parse (표준 키)
 *     → strcasestr/spdk_strtol (확장 키 ns/hostnqn/alt_traddr)
 */
int
spdk_nvme_trid_entry_parse(struct spdk_nvme_trid_entry *trid_entry, const char *str)
{
	struct spdk_nvme_transport_id *trid;
	/* [한국어] trid_entry 내부의 trid 필드 alias - 코드 가독성용 단축 포인터. */

	char *ns, *hostnqn, *alt_traddr;
	/* [한국어] strcasestr 결과 - 각각 "ns:", "hostnqn:", "alt_traddr:" 매치 시작 위치.
	 * 미발견 시 NULL이며 if 조건의 제3 형식(`||`로 ':'/'=' 둘 다 시도)에서 NULL 검사. */

	size_t len;
	/* [한국어] strcspn 결과 - 다음 공백/탭/개행까지의 value 길이. 검증 및 복사 길이로 사용. */

	trid = &trid_entry->trid;
	/* [한국어] 출력 구조체 내부 trid 가리킴 - 표준 파서에 그대로 넘길 수 있도록. */

	trid->trtype = SPDK_NVME_TRANSPORT_PCIE;
	/* [한국어] trtype 기본값 - 사용자가 trtype 키를 생략했다면 PCIe로 간주.
	 * 가장 흔한 사용 사례(로컬 PCIe NVMe SSD)에 대한 편의 default. */

	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	/* [한국어] subnqn 기본값 - 사용자가 subnqn 생략 시 NVMe-oF 디스커버리 controller로 연결.
	 * snprintf로 sizeof 한계까지 안전 복사 (truncation OK). */

	if (spdk_nvme_transport_id_parse(trid, str) != 0) {
		/* [한국어] 표준 키(trtype/adrfam/traddr/trsvcid/subnqn) 파싱 - nvme_transport.c 정의.
		 * 미인식 키는 silently 무시 (그래서 ns/hostnqn/alt_traddr를 아래에서 따로 처리). */
		SPDK_ERRLOG("Invalid transport ID format '%s'\n", str);
		return -EINVAL;
	}

	if ((ns = strcasestr(str, "ns:")) ||
	    (ns = strcasestr(str, "ns="))) {
		/* [한국어] case-insensitive로 "ns:" 또는 "ns=" 검색. 둘 다 안 나오면 if 진입 안 함.
		 * GNU strcasestr 사용 - 표준 C 아님 (POSIX 확장).
		 * 한 표현식에 두 호출이라 첫 번째가 NULL이면 두 번째를 시도하고 ns에 결과 대입. */

		char nsid_str[6]; /* 5 digits maximum in an nsid */
		/* [한국어] nsid 텍스트 임시 버퍼 - 65535는 5자리, +1 = 6 (null terminator). */

		int nsid;
		/* [한국어] 변환된 정수 nsid (uint16_t로 캐스팅 전 음수 검증 위해 int로). */

		ns += 3;
		/* [한국어] "ns:" 또는 "ns=" 자체를 건너뛰고 value 시작 위치로 이동 (3 = "ns:" 길이). */

		len = strcspn(ns, " \t\n");
		/* [한국어] value 끝(공백/탭/개행) 직전까지의 길이 측정. " ns:1 hostnqn:..." 같은 경우
		 * 1까지의 길이만 추출. */

		if (len > 5) {
			/* [한국어] 5자리(=99999) 초과면 65535 max를 넘으므로 즉시 거부. */
			SPDK_ERRLOG("NVMe namespace IDs must be 5 digits or less\n");
			return -EINVAL;
		}

		memcpy(nsid_str, ns, len);
		/* [한국어] value 부분만 임시 버퍼에 복사 - 원본 str 변경 없이. */

		nsid_str[len] = '\0';
		/* [한국어] null terminator 강제 - spdk_strtol에 안전한 C string 전달. */

		nsid = spdk_strtol(nsid_str, 10);
		/* [한국어] base 10 십진수 변환 - SPDK 안전 래퍼 (strtol+errno 안전 처리,
		 * 음수/오버플로 시 음수 errno 반환). */

		if (nsid <= 0 || nsid > 65535) {
			/* [한국어] NVMe namespace ID 유효 범위는 1~65535 (NVMe 1.x 스펙).
			 * 0은 invalid, 65536 이상은 16비트 초과. spdk_strtol 에러도 음수 반환되어 여기서 잡힘. */
			SPDK_ERRLOG("NVMe namespace IDs must be less than 65536 and greater than 0\n");
			return -EINVAL;
		}

		trid_entry->nsid = (uint16_t)nsid;
		/* [한국어] 검증 후 uint16_t로 안전 캐스팅. */
	}

	if ((hostnqn = strcasestr(str, "hostnqn:")) ||
	    (hostnqn = strcasestr(str, "hostnqn="))) {
		/* [한국어] hostnqn 키 검색 - NVMe-oF에서 호스트를 식별하는 NQN. */

		hostnqn += strlen("hostnqn:");
		/* [한국어] "hostnqn:" 길이(8)만큼 건너뛰어 value로 이동. ':'와 '='는 길이 같음. */

		len = strcspn(hostnqn, " \t\n");
		/* [한국어] NQN value 끝까지 길이 측정. NQN은 보통 reverse-DNS 형식이라 길 수 있음. */

		if (len > (sizeof(trid_entry->hostnqn) - 1)) {
			/* [한국어] 출력 버퍼 크기 - null 자리 1바이트 = 실제 저장 가능 NQN 길이.
			 * 보통 hostnqn[SPDK_NVMF_NQN_MAX_LEN+1]로 정의됨 (NQN 최대 223자). */
			SPDK_ERRLOG("Host NQN is too long\n");
			return -EINVAL;
		}

		memcpy(trid_entry->hostnqn, hostnqn, len);
		/* [한국어] NQN value 복사 - 원본 str에서 trid_entry로. */

		trid_entry->hostnqn[len] = '\0';
		/* [한국어] null terminator 보장. */
	}

	trid_entry->failover_trid = trid_entry->trid;
	/* [한국어] 구조체 전체 복사 (struct assignment) - alt_traddr가 없으면 failover는 primary와 동일.
	 * 있으면 아래에서 traddr 필드만 덮어쓰기. */

	if ((alt_traddr = strcasestr(str, "alt_traddr:")) ||
	    (alt_traddr = strcasestr(str, "alt_traddr="))) {
		/* [한국어] failover 대체 주소 키 검색 - multipath 시나리오. */

		alt_traddr += strlen("alt_traddr:");
		/* [한국어] "alt_traddr:" 길이(11) 건너뛰어 value로. */

		len = strcspn(alt_traddr, " \t\n");
		/* [한국어] traddr value 길이 측정. */

		if (len > SPDK_NVMF_TRADDR_MAX_LEN) {
			/* [한국어] NVMe-oF traddr 최대 길이 검증 (스펙 정의). */
			SPDK_ERRLOG("The failover traddr %s is too long.\n", alt_traddr);
			return -EINVAL;
		}

		snprintf(trid_entry->failover_trid.traddr, SPDK_NVMF_TRADDR_MAX_LEN + 1, "%s", alt_traddr);
		/* [한국어] failover_trid의 traddr만 alt 주소로 덮어쓰기. snprintf로 안전 복사
		 * (MAX_LEN+1로 null 자리 포함). 다른 필드(trtype/adrfam/trsvcid/subnqn)는
		 * primary와 동일하게 유지됨 - 같은 subsystem의 다른 path만 바뀐다는 가정. */
	}

	return 0;
	/* [한국어] 모든 파싱 성공. trid_entry는 사용자가 spdk_nvme_probe에 넘길 준비 완료. */
}

/*
 * [한국어]
 * spdk_nvme_build_name - controller(+namespace)를 사람이 읽을 수 있는 이름 문자열로 직렬화
 *
 * @name:   결과를 쓸 출력 버퍼.
 * @length: 출력 버퍼 크기 (null terminator 포함).
 * @ctrlr:  이름을 만들 NVMe controller (필수, 최소 trid 추출 가능 상태).
 * @ns:     namespace 포인터 (선택, NULL이면 controller만 표기, non-NULL이면 " NSID <id>" 추가).
 *
 * @return: 성공 시 쓴 바이트 수 (null 제외), 실패 시 음수 (-EINVAL = unknown trtype, snprintf 에러).
 *
 * 왜 필요한가: 로그/통계/UI에 controller를 식별 가능한 이름으로 표시 필요.
 *   PCIe는 BDF + vendor:device, Fabric은 IP+subnqn, vfio-user는 socket path 등 트랜스포트별
 *   포맷이 다르므로 통일된 빌더 함수가 유용.
 *
 * 동작 단계:
 *   1) ctrlr에서 trid 추출.
 *   2) trtype switch:
 *      - PCIE: "PCIE (BDF) [vendor:device]" - PCI ID는 best-effort 추가.
 *      - RDMA/TCP: "<TYPE> (addr:IP subnqn:NQN)".
 *      - VFIOUSER/CUSTOM: "<TYPE> (path)".
 *      - default: stderr 경고 + -EINVAL.
 *   3) ns가 있으면 " NSID <id>" 이어 붙임.
 *   4) 총 쓴 바이트 수 반환.
 *
 * 실행 컨텍스트: 보통 attach 콜백 또는 통계 출력 경로의 메인 스레드. snprintf 다중 호출.
 *   I/O 경로 아니므로 성능 영향 없음.
 *
 * 주의: snprintf 반환값 처리 - 음수면 인코딩 에러, length 이상이면 truncation. 본 함수는
 *   length 초과는 검사하지 않고 단순 누적 (호출자가 충분한 길이 보장 책임).
 *
 * caller: nvme_attach_cb, perf 통계 출력, bdev_nvme controller 이름 빌드 등.
 * callee: spdk_nvme_ctrlr_get_transport_id, spdk_nvme_ctrlr_get_pci_device,
 *   spdk_pci_device_get_id, spdk_nvme_ns_get_id, snprintf, fprintf.
 *
 * 호출 체인:
 *   attach_cb / stats_print → [spdk_nvme_build_name]
 *     → spdk_nvme_ctrlr_get_transport_id (nvme_ctrlr.c)
 *     → (PCIe) spdk_nvme_ctrlr_get_pci_device → spdk_pci_device_get_id (env_dpdk)
 *     → (ns 있을 시) spdk_nvme_ns_get_id (nvme_ns.c)
 */
int
spdk_nvme_build_name(char *name, size_t length, struct spdk_nvme_ctrlr *ctrlr,
		     struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_transport_id *trid;
	/* [한국어] ctrlr의 트랜스포트 식별 정보 - trtype, traddr, subnqn 등을 보유.
	 * const라 읽기 전용. */

	int res, res2 = 0;
	/* [한국어] res = trtype별 메인 부분 snprintf 결과 (쓴 바이트 수 또는 음수 에러).
	 * res2 = ns가 있을 때 " NSID X" 추가 부분의 결과. ns 없으면 0 유지 → 합산 시 무영향. */

	trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);
	/* [한국어] ctrlr 내부에 보관된 trid 포인터 반환 - 복사 없음, lifetime은 ctrlr와 동일. */

	switch (trid->trtype) {
	case SPDK_NVME_TRANSPORT_PCIE: {
		/* [한국어] 로컬 PCIe NVMe SSD - 가장 정보가 풍부한 케이스 (BDF + vendor/device ID). */

		struct spdk_pci_device *dev;
		/* [한국어] PCIe 디바이스 핸들 - DPDK PCI subsystem이 관리. */

		res = snprintf(name, length, "PCIE (%s)", trid->traddr);
		/* [한국어] 기본 라벨 - "PCIE (0000:04:00.0)" 형태. traddr는 BDF 문자열. */

		dev = spdk_nvme_ctrlr_get_pci_device(ctrlr);
		/* [한국어] PCI 디바이스 핸들 추출 - PCIe 트랜스포트만 가능. failover 등에서는 NULL 가능. */

		if (dev && res > 0) {
			/* [한국어] PCI 디바이스 있고 base 이름 쓰기 성공한 경우만 추가 (best-effort). */

			struct spdk_pci_id pci_id;
			/* [한국어] PCI ID 4종 - vendor_id, device_id, subvendor_id, subdevice_id. */

			int _res;
			/* [한국어] 추가 snprintf 결과 임시 저장 - res에 누적 전 검사용. */

			pci_id = spdk_pci_device_get_id(dev);
			/* [한국어] 디바이스에서 PCI ID 추출 - PCI config space 읽기 또는 캐시 조회. */

			_res = snprintf(name + res, length - res, " [%04x:%04x]", pci_id.vendor_id, pci_id.device_id);
			/* [한국어] 기존 라벨 뒤에 " [vendor:device]" 추가 - 4자리 hex (예: " [8086:0a54]").
			 * name + res로 이어 쓰기, length - res로 남은 공간만 사용. */

			if (_res > 0) {
				/* [한국어] truncation 또는 에러 시 res 갱신하지 않음 - 전체 길이가
				 * 보수적으로 유지되도록. */
				res = res + _res;
			}
		}
		break;
	}
	case SPDK_NVME_TRANSPORT_RDMA:
		res = snprintf(name, length, "RDMA (addr:%s subnqn:%s)", trid->traddr, trid->subnqn);
		/* [한국어] NVMe-oF RDMA - "RDMA (addr:192.168.1.10 subnqn:nqn.2014-08...)". */
		break;
	case SPDK_NVME_TRANSPORT_TCP:
		res = snprintf(name, length, "TCP (addr:%s subnqn:%s)", trid->traddr, trid->subnqn);
		/* [한국어] NVMe-oF TCP - 형식 동일, 라벨만 다름. */
		break;
	case SPDK_NVME_TRANSPORT_VFIOUSER:
		res = snprintf(name, length, "VFIOUSER (%s)", trid->traddr);
		/* [한국어] VFIO-USER 트랜스포트 - traddr는 보통 unix socket 경로.
		 * subnqn 미사용 (vfio-user는 단일 디바이스). */
		break;
	case SPDK_NVME_TRANSPORT_CUSTOM:
		res = snprintf(name, length, "CUSTOM (%s)", trid->traddr);
		/* [한국어] 사용자 정의 트랜스포트 - 외부 모듈이 spdk_nvme_transport_register로 등록.
		 * traddr 의미는 모듈 정의에 따름. */
		break;
	default:
		fprintf(stderr, "Unknown transport type %d\n", trid->trtype);
		/* [한국어] 알 수 없는 trtype - 새 트랜스포트가 추가됐는데 이 switch 미갱신 시 발생. */
		res = -EINVAL;
	}

	if (res < 0) {
		/* [한국어] snprintf 인코딩 에러 또는 unknown trtype - 즉시 에러 전파, ns 추가 안 함. */
		return res;
	}

	if (ns) {
		/* [한국어] ns 포인터가 non-NULL이면 namespace ID 표기 추가. */

		res2 = snprintf(name + res, length - res, " NSID %u", spdk_nvme_ns_get_id(ns));
		/* [한국어] " NSID 1" 형태 추가. spdk_nvme_ns_get_id는 1-base namespace ID 반환. */
	}

	if (res2 < 0) {
		/* [한국어] NSID 추가 중 에러 - 부분 라벨 무시하고 에러 코드 전파. */
		return res2;
	}

	return res + res2;
	/* [한국어] base 라벨 + (있다면) NSID 추가까지 총 쓴 바이트 수 반환.
	 * 호출자는 이 값으로 truncation 여부 추정 가능 (length 이상이면 truncated). */
}
