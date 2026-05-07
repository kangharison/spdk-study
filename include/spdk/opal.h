/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] TCG Opal SED 공개 API (opal.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 TCG Storage Architecture Core Specification (Storage Specification, SSC)에 정의된
 * Opal SED(Self-Encrypting Drive)를 SPDK의 NVMe 컨트롤러를 통해 제어하기 위한 **공개 API**이다.
 * SED는 디스크 자체가 AES 엔진을 내장해 모든 사용자 데이터를 암호화하는 하드웨어 기능으로,
 * 호스트는 PIN(passphrase)을 가진 권한자(authority)로서 잠금/해제, locking range 설정,
 * 사용자 추가/삭제, secure erase 등을 명령한다. 이 헤더는 spdk_opal_dev라는 불투명 핸들
 * (실제 정의는 lib/nvme/nvme_opal.c)와 16개 가량의 spdk_opal_cmd_* 동기 명령 함수를 노출한다.
 * 와이어 포맷(opal_spec.h, TCG SWG token stream — Start Session, Method Call 등)은 여기서
 * 직접 노출하지 않으며, 사용자는 단순한 동기 함수 호출로 SED 보안 동작을 수행할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 API는 SPDK 보안 스택의 최상단(공개 헤더)이다.
 *   상위 호출자: nvme-cli의 nvme-opal subcommand, NVMe-oF target 보안 설정 코드,
 *      app/spdk_top 등 관리 도구. 호출은 SPDK 어플리케이션 스레드(spdk_thread) 컨텍스트에서 이뤄진다.
 *   본 파일: spdk_opal_dev * 핸들과 함수 시그니처만 선언.
 *   하위 구현: lib/nvme/nvme_opal.c — TCG SWG 토큰을 빌드하고
 *      spdk_nvme_ctrlr_security_send/receive(nvme.h)로 전송. 응답을 파싱해 호출자에게 int 반환.
 *   더 하위: NVMe Security Send (opcode 0x81) / Security Receive (opcode 0x82) Admin 명령.
 *      Protocol ID = 0x01 (TCG), Comm ID = 패킷 헤더의 송수신 ID.
 *   실행 컨텍스트: 호스트 유저스페이스. Admin qpair는 spdk_nvme_ctrlr_process_admin_completions()
 *      가 폴링하므로, 동기 함수는 응답이 올 때까지 내부 폴링 루프로 대기한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/stdinc.h: uint64_t/bool 등 기본 타입
 *   - spdk/nvme.h: spdk_nvme_ctrlr (Opal dev 생성 시 컨트롤러 핸들 필요)
 *   - spdk/log.h: 내부 디버그/에러 로그 (구현부에서 사용)
 *   - spdk/endian.h: TCG 토큰의 big-endian 표현 변환
 *   - spdk/string.h: passwd 처리 등 문자열 유틸
 *   - spdk/opal_spec.h: TCG D0 Level 0 Discovery feature 구조체들
 *     (struct spdk_opal_d0_tper_feat 등 — Discovery 응답 파싱 결과)
 * 의존되는 곳:
 *   - lib/nvme/nvme_opal.c: 본 헤더의 모든 함수의 실제 구현
 *   - lib/nvme/nvme_opal_internal.h: spdk_opal_dev 내부 구조 정의
 *   - module/bdev/nvme/bdev_nvme.c: NVMe bdev에 SED 잠금 해제 RPC 노출
 * 데이터 흐름:
 *   사용자 PIN(char *passwd) → spdk_opal_cmd_*() → TCG 토큰 빌드(nvme_opal.c)
 *     → Security Send(0x81)로 SSD 전송 → SSD가 인증/처리 → Security Receive(0x82)로 응답 회수
 *     → 토큰 디코딩 → 호출자에게 int 반환(0=성공, 음수=errno 또는 TCG 에러 코드)
 * 공유 자료구조: spdk_opal_dev는 컨트롤러 1개당 하나 — Discovery 결과(Level 0 features),
 *   세션 컨텍스트(host session ID, TPer session ID), locking range info 캐시를 보유.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_opal_dev_construct/destruct: NVMe 컨트롤러를 입력으로 Opal 핸들 생성/소멸.
 *     생성 시 Level 0 Discovery (IF-RECV protocol 0x01, comID 0x0001) 수행해 D0 feature 캐시.
 * - spdk_opal_get_d0_features_info: 위 캐시된 Discovery 결과를 조회.
 * - spdk_opal_cmd_take_ownership: 공장 출하 SID PIN(=MSID)을 사용자가 정한 새 PIN으로 변경.
 *     Opal 활성화의 첫 단계 (TCG SSC §3.1).
 * - spdk_opal_cmd_revert_tper: TPer를 공장 초기 상태로 되돌림 (모든 PIN/range 삭제 + crypto erase).
 * - spdk_opal_cmd_activate_locking_sp: SP가 Manufactured-Inactive → Manufactured 상태로 전이.
 * - spdk_opal_cmd_lock_unlock: 특정 user/range를 RO/RW/locked 상태로 변경 (가장 자주 쓰이는 API).
 * - spdk_opal_cmd_setup_locking_range: locking range의 LBA 시작/길이 정의.
 * - spdk_opal_cmd_get_max_ranges / get_locking_range_info: SSD가 지원하는 range 수, 상태 조회.
 * - spdk_opal_cmd_enable_user / add_user_to_locking_range / set_new_passwd: user 권한자 관리.
 * - spdk_opal_cmd_erase_locking_range / secure_erase_locking_range: range 데이터 삭제(키 폐기).
 * - struct spdk_opal_d0_features_info: Level 0 Discovery 결과 7종 feature 묶음.
 * - struct spdk_opal_locking_range_info: 한 locking range의 LBA/락 상태 캐시.
 * - enum spdk_opal_lock_state, spdk_opal_user, spdk_opal_locking_range: API 인자용 식별자 enum.
 */

#ifndef SPDK_OPAL_H
/* [한국어] 다중 포함 방지 가드. SPDK 공개 헤더 표준 패턴. */
#define SPDK_OPAL_H

#include "spdk/stdinc.h"
/* [한국어] uint8_t/uint64_t/bool 등 기본 타입과 NULL/size_t 가져옴. */
#include "spdk/nvme.h"
/* [한국어] spdk_nvme_ctrlr 전방 선언과 spdk_nvme_ctrlr_security_send/receive() 시그니처.
 * Opal은 NVMe Security Send(0x81)/Receive(0x82)를 운반체로 사용하므로 필수. */
#include "spdk/log.h"
/* [한국어] SPDK_ERRLOG/SPDK_DEBUGLOG 등 — 본 헤더 자체는 호출하지 않지만,
 * 인라인 코드 또는 내부 헤더 의존성을 만족시키기 위해 포함. */
#include "spdk/endian.h"
/* [한국어] from_be16/from_be64 등 빅엔디안 변환 매크로 — TCG SWG 토큰은 big-endian. */
#include "spdk/string.h"
/* [한국어] 패스워드 길이 측정/안전 복사 등 문자열 유틸 — 구현부 인라인 helper용. */
#include "spdk/opal_spec.h"
/* [한국어] TCG Opal Level 0 Discovery feature 구조체 정의:
 * struct spdk_opal_d0_tper_feat (TPer feature: Sync/AsyncSupport/AckNak 등),
 * struct spdk_opal_d0_locking_feat (Locking feature: Locked/LockingEnabled/MBR 비트),
 * struct spdk_opal_d0_geo_feat (geometry: LogicalBlockSize, AlignmentGranularity),
 * struct spdk_opal_d0_v100_feat (Opal SSC v1.00),
 * struct spdk_opal_d0_v200_feat (Opal SSC v2.00 — 본 SPDK 구현 대상). */

#ifdef __cplusplus
/* [한국어] C++에서 포함될 때 심볼 맹글링 방지. */
extern "C" {
#endif

struct spdk_opal_d0_features_info {
	/* [한국어] TCG Level 0 Discovery (IF-RECV protocol 0x01 comID 0x0001) 응답을 파싱해
	 * 7개 feature descriptor를 한 묶음으로 보관하는 호스트측 캐시 구조체.
	 * 설정자: spdk_opal_dev_construct() 시점에 nvme_opal.c가 채움.
	 * 읽는 자: 사용자가 spdk_opal_get_d0_features_info(dev)로 접근해 SSD 능력 검사.
	 * 동기화: 한 번 채운 뒤 read-only — 별도 락 불필요. */

	struct spdk_opal_d0_tper_feat tper;
	/* [한국어] TPer Feature(코드 0x0001) — Trusted Peripheral 기본 능력 비트맵.
	 * Sync/Async/AckNak/Buffer Mgmt/Streaming/ComID Mgmt 지원 여부 포함.
	 * 설정자: spdk_opal_dev_construct 내부 Discovery 응답 파서.
	 * 읽는 자: nvme_opal.c가 Sync/Async 패킷 모드를 결정할 때 + 사용자가 디바이스 호환성 검사.
	 * 값 범위: spdk_opal_d0_tper_feat 비트 필드(opal_spec.h 정의) — 0/1 조합.
	 * 동기화: construct 후 read-only. */

	struct spdk_opal_d0_locking_feat locking;
	/* [한국어] Locking Feature(코드 0x0002) — Locking SP 활성화/잠금 상태/MBR Shadow 활성화 비트.
	 * 설정자: Discovery 응답 파서.
	 * 읽는 자: 사용자가 LockingEnabled 비트 검사 — 0이면 take_ownership/activate_locking_sp가
	 *   선행되어야 함을 의미.
	 * 값 범위: spdk_opal_d0_locking_feat 비트 필드 — LockingEnabled, Locked, MBREnabled,
	 *   MBRDone, MBRShadowing 등.
	 * 동기화: construct 후 read-only. 단, 실제 Locked/Done 비트는 디바이스 측에서 동적이므로
	 *   런타임 정확도가 필요하면 별도 Discovery를 재발행해야 함(현재 API 미노출). */

	struct spdk_opal_d0_single_user_mode_feat single_user;
	/* [한국어] Single User Mode Feature(코드 0x0201) — 1 사용자 = 1 range로 단순화한 모드.
	 * 설정자: Discovery 응답 파서.
	 * 읽는 자: 사용자가 SUM 활성 여부 확인. SUM 모드에서는 사용자 추가/range 분할 등이
	 *   다르게 동작하므로 사전 검사 필요.
	 * 값 범위: SUM 활성 여부, 자동 lock 정책, 잠금 범위 수 등 비트/정수 필드.
	 * 동기화: construct 후 read-only. */

	struct spdk_opal_d0_geo_feat geo;
	/* [한국어] Geometry Feature(코드 0x0003) — 논리 블록 크기, alignment granularity, lowest aligned LBA.
	 * 설정자: Discovery 응답 파서.
	 * 읽는 자: 사용자가 setup_locking_range 호출 시 range_start/length를 alignment에 맞춰
	 *   계산하기 위해 참조. nvme_opal.c도 일부 검사에 사용 가능.
	 * 값 범위: LogicalBlockSize(보통 512 또는 4096), AlignmentGranularity(LBA 배수),
	 *   LowestAlignedLBA(시작 위치).
	 * 동기화: construct 후 read-only. */

	struct spdk_opal_d0_datastore_feat datastore;
	/* [한국어] DataStore Table Feature(코드 0x0202) — 호스트가 임의 데이터를 저장할 수 있는
	 * 공간(테이블 크기/개수).
	 * 설정자: Discovery 응답 파서.
	 * 읽는 자: nvme-cli의 datastore put/get에 사용 — 본 SPDK API는 직접 노출 X.
	 * 값 범위: MaxNumDataStoreTables, MaxSizeOfDataStoreTables 등 정수 필드.
	 * 동기화: construct 후 read-only. */

	struct spdk_opal_d0_v100_feat v100;
	/* [한국어] Opal SSC v1.00 Feature(코드 0x0200) — base comID, 사용자/locking range 한도.
	 * 설정자: Discovery 응답 파서. 펌웨어가 v1.00 호환이면 채워짐, 아니면 0으로 둠.
	 * 읽는 자: nvme_opal.c가 v200이 없을 때 fallback으로 base_comid 추출.
	 * 값 범위: BaseComID, NumComIDs, num_locking_admin_authorities, num_locking_user_authorities.
	 *   펌웨어가 v1.00 모드만 지원하면 v2.00 비기능적 비트는 의미 없음.
	 * 동기화: construct 후 read-only. */

	struct spdk_opal_d0_v200_feat v200;
	/* [한국어] Opal SSC v2.00 Feature(코드 0x0203) — base comID, 사용자/admin 수, RangeCrossing.
	 * 설정자: Discovery 응답 파서. SPDK는 v2.00 호환 SED를 주 타깃으로 함.
	 * 읽는 자: nvme_opal.c가 v200.base_comid를 모든 SWG 명령의 ComID로 사용.
	 *   사용자가 num_users/num_admins로 enable_user 호출 가능 한도 결정.
	 * 값 범위: BaseComID, NumComIDs, RangeCrossingBehavior, NumLockingSPAdminAuth(보통 4),
	 *   NumLockingSPUserAuth(보통 9). 사용자 ID 매핑(OPAL_USER1..9)이 여기 정의됨.
	 * 동기화: construct 후 read-only. */
};

enum spdk_opal_lock_state {
	/* [한국어] spdk_opal_cmd_lock_unlock의 flag 인자로 사용되는 잠금 상태 enum.
	 * TCG SWG 메서드 SetLocking_*에 들어가는 ReadLocked/WriteLocked 비트와 매핑된다.
	 * 설정자: API 호출자. 읽는 자: nvme_opal.c가 토큰 빌드 시 비트로 변환. */

	OPAL_READONLY	= 0x01,
	/* [한국어] WriteLocked=true, ReadLocked=false → 읽기만 허용, 쓰기 차단.
	 * 설정자: API 호출자(spdk_opal_cmd_lock_unlock의 flag 인자).
	 * 읽는 자: nvme_opal.c가 LockingRange[id].WriteLocked=true / ReadLocked=false 비트로 인코딩.
	 * 값 범위: 정수 0x01 — 본 enum 내부 식별자(TCG 와이어 인코딩과 직접 매핑되지 않음).
	 * 동기화: enum 자체는 const — 무관. 실제 잠금 비트 변경은 SP 단일 세션으로 직렬화.
	 * 사용 사례: 부팅 후 OS가 read-only로 마운트할 때, 또는 백업 도구가 데이터 변조 방지하며 read만 할 때. */

	OPAL_RWLOCK		= 0x02,
	/* [한국어] WriteLocked=true, ReadLocked=true → 모든 I/O 차단(완전 잠금).
	 * 설정자: API 호출자.
	 * 읽는 자: nvme_opal.c가 두 잠금 비트를 모두 set으로 인코딩.
	 * 값 범위: 정수 0x02.
	 * 동기화: OPAL_READONLY와 동일.
	 * 사용 사례: 시스템 셧다운/슬립 진입 시, 또는 분실 보호 모드. */

	OPAL_READWRITE	= 0x04,
	/* [한국어] WriteLocked=false, ReadLocked=false → 정상 읽기/쓰기 허용 (잠금 해제).
	 * 설정자: API 호출자.
	 * 읽는 자: nvme_opal.c가 두 잠금 비트를 모두 clear로 인코딩.
	 * 값 범위: 정수 0x04.
	 * 동기화: OPAL_READONLY와 동일.
	 * 사용 사례: 사용자가 PIN으로 인증 후 OS 부팅 시 unlock에 사용 — 가장 빈번. */
};

enum spdk_opal_user {
	/* [한국어] Opal SP의 권한자(Authority) 식별자 — Locking SP 내 user/admin 슬롯.
	 * TCG Opal v2.00은 Admin 1개 + User 1~9 (총 10명)를 표준 정의.
	 * 설정자: API 호출자. 읽는 자: nvme_opal.c가 권한자 UID(8바이트)로 변환해
	 *   StartSession 메서드의 HostSigningAuthority 파라미터에 인코딩. */

	OPAL_ADMIN1 = 0x0,
	/* [한국어] Locking SP Admin1 — Locking SP의 슈퍼유저(첫 번째 admin 권한자).
	 * 설정자: API 호출자가 user 인자로 지정.
	 * 읽는 자: nvme_opal.c가 Authority UID로 변환(Admin1 UID = 0x00000009-0001-0001-0000-000000000001 형태).
	 * 값 범위: 정수 0x0. take_ownership 후 첫 PIN 보유자가 이 권한자.
	 *   사용자 추가/삭제, locking range 정의, secure erase 등 모든 관리 작업 가능.
	 * 동기화: enum 자체는 const. 실제 Admin1 PIN 변경은 SP 단일 세션 직렬화. */

	OPAL_USER1 = 0x01,
	/* [한국어] User1 — 일반 사용자 권한자(첫 번째 user 슬롯).
	 * 설정자: API 호출자.
	 * 읽는 자: nvme_opal.c가 User1 UID로 변환.
	 * 값 범위: 정수 0x01. enable_user로 활성화 후 set_new_passwd로 PIN 설정 필요.
	 *   add_user_to_locking_range로 특정 range의 BooleanACE에 추가되어야 lock/unlock 가능.
	 * 동기화: enum 자체는 const. 실제 활성화/PIN/ACE 변경은 SP 단일 세션 직렬화. */

	OPAL_USER2 = 0x02,
	/* [한국어] User2 — 두 번째 일반 사용자 권한자 슬롯.
	 * 설정자: API 호출자가 spdk_opal_cmd_enable_user/add_user_to_locking_range/set_new_passwd
	 *   호출 시 user_id 인자로 지정.
	 * 읽는 자: nvme_opal.c가 TCG SWG 메서드의 HostSigningAuthority UID 인코딩 시 매핑
	 *   (Authority 테이블 행: User2 → UID 0x00000009-0001-0001-0000-000000000003 형태).
	 * 값 범위: 정수 0x02 — Authority 테이블의 user 인덱스 + 1과 동일.
	 * 동기화: enum 자체는 const — 동시성 무관. 단, 같은 user_id로 동시 enable/PIN 변경
	 *   호출 시 SSD 펌웨어 측 직렬화에 의존(SP 단위 단일 세션 제약). */

	OPAL_USER3 = 0x03,
	/* [한국어] User3 — 세 번째 일반 사용자 권한자 슬롯.
	 * 설정자/읽는 자: OPAL_USER2와 동일 패턴.
	 * 값 범위: 0x03. v200.num_users가 3 이상이어야 펌웨어가 수락.
	 * 동기화: OPAL_USER2와 동일. */

	OPAL_USER4 = 0x04,
	/* [한국어] User4 — 네 번째 사용자 슬롯.
	 * 설정자/읽는 자: OPAL_USER2와 동일 패턴.
	 * 값 범위: 0x04. v200.num_users >= 4 필요.
	 * 동기화: OPAL_USER2와 동일. */

	OPAL_USER5 = 0x05,
	/* [한국어] User5 — 다섯 번째 사용자 슬롯.
	 * 설정자/읽는 자: OPAL_USER2와 동일 패턴.
	 * 값 범위: 0x05. v200.num_users >= 5 필요.
	 * 동기화: OPAL_USER2와 동일. */

	OPAL_USER6 = 0x06,
	/* [한국어] User6 — 여섯 번째 사용자 슬롯.
	 * 설정자/읽는 자: OPAL_USER2와 동일 패턴.
	 * 값 범위: 0x06. v200.num_users >= 6 필요.
	 * 동기화: OPAL_USER2와 동일. */

	OPAL_USER7 = 0x07,
	/* [한국어] User7 — 일곱 번째 사용자 슬롯.
	 * 설정자/읽는 자: OPAL_USER2와 동일 패턴.
	 * 값 범위: 0x07. v200.num_users >= 7 필요.
	 * 동기화: OPAL_USER2와 동일. */

	OPAL_USER8 = 0x08,
	/* [한국어] User8 — 여덟 번째 사용자 슬롯.
	 * 설정자/읽는 자: OPAL_USER2와 동일 패턴.
	 * 값 범위: 0x08. v200.num_users >= 8 필요.
	 * 동기화: OPAL_USER2와 동일. */

	OPAL_USER9 = 0x09,
	/* [한국어] User9 — TCG Opal v2.00이 보장하는 마지막 표준 사용자 슬롯.
	 * 설정자: API 호출자가 user_id 인자로 지정.
	 * 읽는 자: nvme_opal.c가 Authority 테이블 UID로 변환.
	 * 값 범위: 0x09 — 펌웨어가 v200.num_users로 보고하는 한도 내에서 사용.
	 *   일부 펌웨어는 num_users < 9이므로 enable_user 호출 전 한도 검사 권장.
	 * 동기화: OPAL_USER2와 동일. */
};

enum spdk_opal_locking_range {
	/* [한국어] Opal locking range 식별자 — 디스크의 LBA 영역을 나누어 각각 다른 잠금 정책 적용.
	 * Global range는 항상 존재하며 디스크 전체를 커버, 1~10번 range는 사용자가 정의.
	 * 설정자: API 호출자. 읽는 자: nvme_opal.c가 LockingRange UID(LR_global, LR_1..10)로 변환. */

	OPAL_LOCKING_RANGE_GLOBAL = 0x0,
	/* [한국어] Global locking range — 디스크 전체. 다른 range가 정의되지 않은 모든 LBA를 커버.
	 * 단순한 Full Disk Encryption 모드에서는 이 range만 사용. */

	OPAL_LOCKING_RANGE_1,
	/* [한국어] Locking Range 1 — 사용자가 정의하는 첫 번째 비-global range.
	 * 설정자: API 호출자가 spdk_opal_cmd_setup_locking_range/lock_unlock 인자로 지정.
	 * 읽는 자: nvme_opal.c가 LockingRange UID(LR_1 = 0x00000008-0002-0001-0000-000000000001)
	 *   로 매핑해 SWG 메서드 호출 대상 객체 식별.
	 * 값 범위: 정수 1 — global=0 다음 인덱스. setup_locking_range로 RangeStart/Length가
	 *   정의되기 전까지는 lock_unlock 호출 시 INVALID_PARAMETER 반환.
	 * 동기화: enum 자체는 const — 동시성 무관. 단일 range에 대한 동시 메서드 호출은
	 *   SSD 펌웨어 측 SP 단일 세션 제약으로 직렬화됨. */

	OPAL_LOCKING_RANGE_2,
	/* [한국어] Locking Range 2 — 두 번째 비-global range.
	 * 설정자/읽는 자: OPAL_LOCKING_RANGE_1과 동일 패턴, UID는 LR_2.
	 * 값 범위: 정수 2. SSD가 지원하는 최대 range 수(get_max_ranges)를 초과하면
	 *   setup_locking_range가 거부됨.
	 * 동기화: OPAL_LOCKING_RANGE_1과 동일. */

	OPAL_LOCKING_RANGE_3,
	/* [한국어] Locking Range 3 — 세 번째 비-global range.
	 * 설정자/읽는 자: 동일 패턴, UID는 LR_3.
	 * 값 범위: 정수 3.
	 * 동기화: OPAL_LOCKING_RANGE_1과 동일. */

	OPAL_LOCKING_RANGE_4,
	/* [한국어] Locking Range 4 — 네 번째 비-global range.
	 * 설정자/읽는 자: 동일 패턴, UID는 LR_4.
	 * 값 범위: 정수 4.
	 * 동기화: OPAL_LOCKING_RANGE_1과 동일. */

	OPAL_LOCKING_RANGE_5,
	/* [한국어] Locking Range 5 — 다섯 번째 비-global range.
	 * 설정자/읽는 자: 동일 패턴, UID는 LR_5.
	 * 값 범위: 정수 5.
	 * 동기화: OPAL_LOCKING_RANGE_1과 동일. */

	OPAL_LOCKING_RANGE_6,
	/* [한국어] Locking Range 6 — 여섯 번째 비-global range.
	 * 설정자/읽는 자: 동일 패턴, UID는 LR_6.
	 * 값 범위: 정수 6.
	 * 동기화: OPAL_LOCKING_RANGE_1과 동일. */

	OPAL_LOCKING_RANGE_7,
	/* [한국어] Locking Range 7 — 일곱 번째 비-global range.
	 * 설정자/읽는 자: 동일 패턴, UID는 LR_7.
	 * 값 범위: 정수 7.
	 * 동기화: OPAL_LOCKING_RANGE_1과 동일. */

	OPAL_LOCKING_RANGE_8,
	/* [한국어] Locking Range 8 — 여덟 번째 비-global range.
	 * 설정자/읽는 자: 동일 패턴, UID는 LR_8.
	 * 값 범위: 정수 8 — Opal v2.00 SSC가 펌웨어에 보장하도록 요구하는 최소치(8개)에 해당.
	 *   따라서 v2.00 호환 SSD라면 이 값까지는 항상 사용 가능.
	 * 동기화: OPAL_LOCKING_RANGE_1과 동일. */

	OPAL_LOCKING_RANGE_9,
	/* [한국어] Locking Range 9 — 아홉 번째 비-global range.
	 * 설정자/읽는 자: 동일 패턴, UID는 LR_9.
	 * 값 범위: 정수 9 — 펌웨어가 9개 이상 지원할 때만 유효(get_max_ranges 결과 확인).
	 * 동기화: OPAL_LOCKING_RANGE_1과 동일. */

	OPAL_LOCKING_RANGE_10,
	/* [한국어] Range 10 — 본 SPDK 헤더가 노출하는 마지막 range 식별자.
	 * 설정자: API 호출자.
	 * 읽는 자: nvme_opal.c가 LR_10 UID로 매핑.
	 * 값 범위: 정수 10. SSD가 지원하는 실제 한도는
	 *   spdk_opal_cmd_get_max_ranges로 조회 가능(펌웨어/제조사에 따라 다름).
	 *   일반적으로 엔터프라이즈 SSD는 더 많은 range를 지원하지만, 본 enum 한도(10)을
	 *   넘는 range는 본 API로 제어 불가 — 직접 nvme_opal.c 내부 함수를 사용해야 함.
	 * 동기화: OPAL_LOCKING_RANGE_1과 동일. */
};

struct spdk_opal_locking_range_info {
	/* [한국어] 단일 locking range의 LBA 정의와 현재 잠금 상태를 캐시하는 구조체.
	 * spdk_opal_cmd_get_locking_range_info() 결과를 보관하며,
	 * spdk_opal_get_locking_range_info()로 조회 / spdk_opal_free_locking_range_info()로 해제.
	 * 설정자: nvme_opal.c가 SSD 응답 파싱 후 채움.
	 * 읽는 자: 사용자가 직접 필드 접근.
	 * 동기화: 사용자가 free 호출 시 nvme_opal.c가 내부 버퍼를 NULL로 표시 — 동시 접근 금지. */

	uint8_t locking_range_id;
	/* [한국어] 어느 range를 가리키는지 식별 (0=global, 1~10=비-global range 번호).
	 * 설정자: nvme_opal.c의 응답 파서가 spdk_opal_cmd_get_locking_range_info() 호출 시 채움.
	 * 읽는 자: 사용자 어플리케이션이 캐시된 range 정보를 매핑할 때.
	 * 값 범위: 0~10 (enum spdk_opal_locking_range와 동일 범위). 펌웨어 한도를 넘는 값은
	 *   cmd가 INVALID_PARAMETER로 거부하므로 여기 들어올 수 없음.
	 * 동기화: 캐시 슬롯은 cmd_get_locking_range_info 동안만 갱신되며, 그 외에는 read-only. */

	uint8_t _padding[7];
	/* [한국어] 64비트 정렬 패딩 — 다음 uint64_t 필드(range_start) alignment 맞춤.
	 * 설정자: 컴파일러 자동 0 초기화(calloc 결과) 또는 명시적 memset.
	 * 읽는 자: 직접 참조하지 않음. 의도적 패딩으로 호스트 시스템 ABI에 무관하게
	 *   일정 레이아웃 유지(추후 ABI 호환성을 위해 명시).
	 * 값 범위: 0(예약). 내용에 의미 부여 금지.
	 * 동기화: read-only — 무관. */

	uint64_t range_start;
	/* [한국어] range의 시작 LBA. global range는 항상 0.
	 * 설정자: SSD 펌웨어가 LockingRange[id].RangeStart 컬럼으로 보고, 응답 파서가 채움.
	 * 읽는 자: 사용자가 LBA 기반 mapping 결정에 사용.
	 * 값 범위: 0 ~ 디바이스 capacity-1. 단위는 논리 블록(geo.LogicalBlockSize).
	 *   geo.AlignmentGranularity 배수여야 setup_locking_range가 수락.
	 * 동기화: cmd_get 호출 시 갱신, 그 외 read-only. */

	uint64_t range_length;
	/* [한국어] range의 길이(LBA 개수). 0이면 미정의(비활성) range.
	 * 설정자: SSD 펌웨어 응답 파서.
	 * 읽는 자: 사용자.
	 * 값 범위: 0 ~ (capacity - range_start). global range는 capacity 전체로 펌웨어가 자동 설정.
	 * 동기화: range_start와 동일. */

	bool read_lock_enabled;
	/* [한국어] 이 range에 read lock 정책이 활성화되어 있는지 (true면 ReadLocked 비트 의미 있음).
	 * 설정자: SSD 펌웨어 (LockingRange[id].ReadLockEnabled 컬럼).
	 * 읽는 자: 호스트가 잠금 해제 절차 분기 결정 — false면 read는 항상 허용되므로
	 *   read_locked 토글이 의미 없음.
	 * 값 범위: true/false. setup_locking_range로 갱신 가능.
	 * 동기화: cmd_get 호출 시 갱신, 그 외 read-only. */

	bool write_lock_enabled;
	/* [한국어] write lock 정책 활성화 여부. read_lock_enabled와 독립적.
	 * 설정자: SSD 펌웨어 (WriteLockEnabled 컬럼).
	 * 읽는 자: 호스트가 write 잠금 정책을 검사할 때.
	 * 값 범위: true/false.
	 * 동기화: read_lock_enabled와 동일. */

	bool read_locked;
	/* [한국어] 현재 read가 잠겨 있는지 (true=차단, false=허용).
	 * 설정자: SSD 펌웨어 (ReadLocked 컬럼) — lock_unlock 호출에 따라 변경.
	 * 읽는 자: 호스트가 OS 부팅 시 unlock 필요 여부 결정.
	 * 값 범위: true/false. read_lock_enabled가 false면 이 비트는 무의미.
	 * 동기화: cmd_get 호출 시 갱신; 그 사이 SSD 측에서 변할 수 있어 stale 가능. */

	bool write_locked;
	/* [한국어] 현재 write가 잠겨 있는지 (true=차단, false=허용).
	 * 설정자: SSD 펌웨어 (WriteLocked 컬럼).
	 * 읽는 자: 호스트가 write 가능 여부 결정.
	 * 값 범위: true/false. write_lock_enabled가 false면 무의미.
	 * 동기화: read_locked와 동일. */
};

struct spdk_opal_dev;
/* [한국어] Opal 장치 핸들 전방 선언 — 구현은 lib/nvme/nvme_opal_internal.h.
 * 호출자는 포인터로만 다루고 내부 필드(세션 ID, comID 등)에 접근 불가 (불투명 핸들 패턴).
 * 한 NVMe 컨트롤러당 0개 또는 1개 — Opal 미지원 컨트롤러는 NULL. */

struct spdk_opal_dev *spdk_opal_dev_construct(struct spdk_nvme_ctrlr *ctrlr);
/* [한국어]
 * spdk_opal_dev_construct - NVMe 컨트롤러로부터 Opal 핸들 생성
 *
 * @ctrlr: spdk_nvme_probe/spdk_nvme_connect로 부착한 활성 컨트롤러 핸들.
 *         NULL이면 NULL 반환.
 * @return: 생성된 Opal 핸들. 컨트롤러가 SED를 미지원하거나 Discovery 실패 시 NULL.
 *          반환된 핸들은 spdk_opal_dev_destruct로 반드시 해제해야 함.
 *
 * 동기/배경: NVMe 컨트롤러를 SED로 제어하려면 사전 Level 0 Discovery로 펌웨어 능력을
 *   확인하고 base comID를 확보해야 한다. 이 함수는 그 한 번뿐인 초기화를 캡슐화한다.
 * 호출 시 내부적으로:
 *   1) calloc으로 spdk_opal_dev 할당 + 컨트롤러 포인터 저장
 *   2) Level 0 Discovery 발행 (Security Receive opcode 0x82, ProtocolID=0x01, ComID=0x0001)
 *   3) 응답 파싱하여 d0_features_info 7개 디스크립터 캐시
 *   4) v200.base_comid를 추출해 이후 모든 SWG 명령에 사용
 * 실행 컨텍스트: 호스트 유저스페이스, SPDK 어플리케이션 스레드. Admin qpair는
 *   spdk_nvme_ctrlr_process_admin_completions()가 폴링하며 본 함수는 그 폴링을
 *   내부적으로 돌려 응답을 동기 회수.
 * 동시성: 한 컨트롤러에 대해 본 함수와 다른 admin 명령을 다른 스레드에서 동시에
 *   호출하면 안 됨 — NVMe 스레드 어피니티(컨트롤러는 한 스레드가 소유) 규칙을 준수해야 함.
 *   본 함수가 반환하기 전에 다른 spdk_opal_cmd_*를 호출하면 정의되지 않은 동작.
 * 에러 경로: ctrlr이 NULL이거나 SED 미지원이면 즉시 NULL. Discovery 응답 파싱 실패,
 *   v200 feature 부재, calloc 실패 시 부분 할당 자원을 해제하고 NULL 반환.
 *   호출자는 NULL 검사로 SED 가용성을 확인해야 함.
 * 호출자(caller): nvme-cli/nvme-opal subcommand, bdev_nvme RPC, 사용자 어플리케이션.
 * 호출 대상(callee): opal_init, opal_discovery0, spdk_nvme_ctrlr_security_receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_dev_construct → opal_init/opal_discovery0 → spdk_nvme_ctrlr_security_receive
 */
void spdk_opal_dev_destruct(struct spdk_opal_dev *dev);
/* [한국어]
 * spdk_opal_dev_destruct - Opal 핸들 해제
 *
 * @dev: spdk_opal_dev_construct가 반환한 핸들. NULL은 무시(no-op).
 * @return: void — 해제 실패 정보를 호출자에게 전달하지 않음(베스트-에포트 정리).
 *
 * 동기/배경: 사용자가 Opal 작업을 모두 끝냈을 때 캐시된 Discovery 결과·세션 컨텍스트·
 *   locking_range_info 슬롯 등을 모두 회수하기 위해 필요.
 * 동작: 활성 세션이 남아 있다면 EndSession 메서드를 보내 정리 시도하고,
 *   실패해도 메모리는 무조건 해제(누수 방지). dev->feat_info, locking_ranges 캐시,
 *   dev 자체를 free.
 * 실행 컨텍스트: 호스트 유저스페이스. 컨트롤러 소유 스레드에서 호출 권장.
 *   spdk_nvme_detach 직전이 가장 일반적인 호출 시점.
 * 동시성: 본 함수 호출 중 다른 spdk_opal_cmd_* 호출 금지 — use-after-free 위험.
 *   호출자가 사전에 다른 스레드의 명령을 quiesce해야 함.
 * 에러 경로: EndSession 실패는 무시(드라이브가 이미 리셋된 상황 등), 메모리 해제만 보장.
 *   호출 후 핸들은 dangling — 호출자는 자신의 포인터를 NULL로 설정해야 함.
 * 호출자: 사용자/nvme-cli 정리 코드, bdev_nvme 모듈 unload 경로.
 * 호출 대상: 내부 EndSession 헬퍼, free.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_dev_destruct → free(dev)
 */

struct spdk_opal_d0_features_info *spdk_opal_get_d0_features_info(struct spdk_opal_dev *dev);
/* [한국어]
 * spdk_opal_get_d0_features_info - 캐시된 Level 0 Discovery 결과 반환
 *
 * @dev: 활성 Opal 핸들. 일반적으로 spdk_opal_dev_construct 후 NULL 아님.
 * @return: dev 내부에 저장된 d0_features_info 포인터 (사용자는 read-only로 사용).
 *          dev이 NULL이면 NULL.
 *
 * 동기/배경: 사용자가 SED의 능력(LockingEnabled 비트, 사용자 수 한도, 논리 블록 크기 등)을
 *   조회해 후속 명령 분기를 결정하기 위해 필요. construct 시점에 한 번만 Discovery를
 *   발행하고 결과를 캐시했으므로 이 함수는 디바이스 왕복 없이 즉시 반환.
 * 동작: 단순히 dev->feat_info의 주소를 반환(NVMe 명령 발행 없음).
 * 실행 컨텍스트: 임의의 SPDK 스레드에서 호출 가능 — read-only 캐시 접근이라 어피니티 무관.
 * 동시성: 캐시는 construct 후 read-only이므로 락 없이 다중 스레드 동시 접근 안전.
 *   단, destruct가 진행 중일 때는 use-after-free 가능 — 호출자가 lifetime을 보장해야 함.
 * 에러 경로: dev이 NULL일 때만 NULL 반환. 그 외 실패 경로 없음(I/O 발생 안 함).
 * 반환 포인터의 수명: spdk_opal_dev_destruct 호출 전까지 유효. 사용자는 포인터를
 *   캐시에 저장해 두지 말고 매번 호출하는 것이 안전.
 * 호출자: 사용자 어플리케이션이 SED 능력 검사 시.
 * 호출 대상: 없음(단순 getter).
 *
 * 호출 체인:
 *   사용자 → spdk_opal_get_d0_features_info → return &dev->feat_info
 */

int spdk_opal_cmd_take_ownership(struct spdk_opal_dev *dev, char *new_passwd);
/* [한국어]
 * spdk_opal_cmd_take_ownership - 공장 출하 PIN(MSID)을 사용자 PIN으로 변경
 *
 * @dev: 활성 Opal 핸들.
 * @new_passwd: 새로 설정할 SID(Locking SP Admin1) PIN. NULL/빈 문자열 거부.
 *   (TCG는 PIN 길이 1~32바이트를 권장. 평문 그대로 와이어로 전송됨.)
 * @return: 0 성공, 음수 errno 또는 TCG 에러 코드(인증 실패 시 ACCESS_DENIED 등).
 *   호출자는 0이 아닌 값에 대해 GUI/로그 표시.
 *
 * 동기/배경: SED는 출하 시 MSID(Manufactured SID — 공개된 디폴트 PIN)로 보호되며,
 *   사용자가 자신만의 PIN을 설정해야 진정한 "소유"가 시작된다. Opal 활성화의 첫 단계.
 *   TCG SSC §3.1.1.4 절차에 해당.
 * 내부 동작:
 *   1) Admin SP에서 MSID 권한자로 StartSession (현재 PIN = MSID, 공장 디폴트)
 *   2) Get MSID 메서드로 32바이트 MSID PIN 회수 (펌웨어가 PSID 라벨 등으로 노출)
 *   3) Set 메서드로 SID PIN을 new_passwd로 갱신
 *   4) EndSession
 * 후속 영향: 이 호출 후 SSD는 사용자가 PIN을 소유한 상태가 되며, 이후 모든 명령은
 *   new_passwd 필요. 잊어버리면 spdk_opal_cmd_revert_tper로 PSID PIN을 사용해야만 복구 가능.
 * 실행 컨텍스트: 동기 — Admin qpair 폴링이 완료될 때까지 함수가 블로킹.
 *   호스트 유저스페이스, 컨트롤러 소유 SPDK 스레드에서 호출.
 * 동시성: 한 dev에 대해 동시에 다른 spdk_opal_cmd_*를 호출하면 안 됨 — SSD 펌웨어는
 *   한 SP에 단일 세션만 허용하므로 두 번째 호출이 SP_BUSY 반환.
 * 에러 경로: NULL/빈 PIN → -EINVAL. StartSession 실패(MSID 변경됐거나 SP 미준비) →
 *   TCG 에러 코드. Set 실패 시 dev 상태 미정의 — 보통 destruct 후 재시도.
 * 호출자: nvme-cli/nvme-opal initialize subcommand, 사용자 어플리케이션 초기 셋업 단계.
 * 호출 대상: opal_start_session, opal_get_msid_cpin_pin, opal_set_sid_cpin_pin,
 *   opal_end_session → spdk_nvme_ctrlr_security_send/receive.
 *
 * 호출 체인:
 *   nvme-cli/사용자 → spdk_opal_cmd_take_ownership → opal_start_session →
 *     spdk_nvme_ctrlr_security_send/receive → SSD 펌웨어
 */

/**
 * synchronous function: send and then receive.
 *
 * Wait until response is received.
 */
/* [한국어] 위 영문 주석 보강: 본 그룹의 spdk_opal_cmd_* 함수들은 모두 동기 모델이다.
 * NVMe Security Send(opcode 0x81)로 TCG SWG 토큰을 보낸 뒤, Security Receive(0x82)로 응답을
 * 폴링·수신하기까지 함수가 블로킹된다. 호출 스레드는 Admin qpair를 폴링하는 컨텍스트여야 하며,
 * 다른 SPDK poller와의 progress 충돌을 피하려면 사용자가 어플리케이션 시작/종료 단계 등
 * 한산한 시점에 호출하는 것이 권장된다. 비동기 콜백 모델은 본 헤더에 노출되지 않는다. */
int spdk_opal_cmd_revert_tper(struct spdk_opal_dev *dev, const char *passwd);
/* [한국어]
 * spdk_opal_cmd_revert_tper - TPer 전체를 공장 초기 상태로 되돌림
 *
 * @dev: 활성 Opal 핸들.
 * @passwd: SID 권한자 PIN(또는 PSID, 펌웨어 정책에 따라).
 * @return: 0 성공, 음수 에러 코드(ACCESS_DENIED 등).
 *
 * 동기/배경: 사용자가 PIN을 잊었거나 SSD를 폐기/재배포할 때 모든 보안 상태와 데이터를
 *   완전히 지우기 위해 필요. take_ownership과 짝을 이루는 "공장 리셋" 동작.
 * 동작: Revert 메서드를 호출해 모든 PIN, locking range, 사용자 권한자, 데이터스토어를
 *   초기화하고 모든 사용자 데이터 영역에 cryptographic erase(미디어 암호 키 폐기)를 수행.
 *   데이터는 즉시 복구 불가 — 매우 파괴적인 명령.
 * 후속 영향: 동작 후 SSD는 take_ownership 이전 상태로 돌아감 (MSID PIN 활성).
 *   호출 후 dev 핸들은 더 이상 유효하지 않을 수 있음 — destruct 후 재construct 권장.
 * 위 영문 주석대로 동기 호출 — 응답 도착까지 함수가 블로킹.
 * 실행 컨텍스트: 호스트 유저스페이스, 컨트롤러 소유 SPDK 스레드. 사용자 명시적 erase 의도 시.
 * 동시성: 본 명령 진행 중 다른 Opal 명령 금지. 또한 진행 중에 일반 NVMe I/O가
 *   수행되면 ACCESS_DENIED나 LBA 잠금 에러로 실패할 수 있어, 호스트가 사전에 I/O를
 *   quiesce하는 것이 권장.
 * 에러 경로: PIN 불일치 → ACCESS_DENIED. Revert 자체는 디바이스 측에서 수 분 걸릴 수 있어
 *   타임아웃에 걸릴 수 있음 — 그 경우 음수 errno. 부분 완료 상태는 펌웨어에 의해
 *   원자적으로 처리되도록 설계됨.
 * 호출자: nvme-cli/nvme-opal psid-revert, 관리 도구의 secure-decommission 흐름.
 * 호출 대상: opal_revert_tper → spdk_nvme_ctrlr_security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_revert_tper → opal_revert_tper → security_send/receive
 */

int spdk_opal_cmd_activate_locking_sp(struct spdk_opal_dev *dev, const char *passwd);
/* [한국어]
 * spdk_opal_cmd_activate_locking_sp - Locking SP를 Manufactured-Inactive → Manufactured 로 활성화
 *
 * @dev: 활성 Opal 핸들.
 * @passwd: Admin SP의 SID 권한자 PIN (take_ownership에서 설정한 값).
 * @return: 0 성공, 음수 에러 코드(ACCESS_DENIED, INVALID_PARAMETER, SP_BUSY 등).
 *
 * 동기/배경: TCG Opal에는 Admin SP(전체 디바이스 보안)와 Locking SP(잠금/암호화) 두
 *   주요 SP(Security Provider)가 있다. Locking SP는 생산 직후 Manufactured-Inactive
 *   상태이며, 이를 Manufactured로 전이해야 locking range 사용/사용자 추가가 가능해진다
 *   (TCG SSC §3.1.1.4).
 * 동작: Admin SP에서 SID로 StartSession → Locking SP 객체에 Activate 메서드 호출 →
 *   EndSession. Activate 후 Locking SP의 Admin1 PIN이 자동으로 SID PIN과 동일하게 설정됨.
 * 후속 영향: 이 호출 이후에야 setup_locking_range/lock_unlock/enable_user 등이 동작.
 * 실행 컨텍스트: 동기, 호스트 유저스페이스, 컨트롤러 소유 스레드.
 * 동시성: 다른 Opal 명령과 병행 금지. SP 단일 세션 제약.
 * 에러 경로: 이미 Manufactured 상태면 INVALID_PARAMETER 가능. PIN 불일치는 ACCESS_DENIED.
 *   실패 시 dev 상태는 변경 없음(SP 전이가 원자적으로 거부됨).
 * 호출자: 사용자 셋업 스크립트(take_ownership 직후).
 * 호출 대상: opal_activate → security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_activate_locking_sp → opal_activate → security_send/receive
 */
int spdk_opal_cmd_lock_unlock(struct spdk_opal_dev *dev, enum spdk_opal_user user,
			      enum spdk_opal_lock_state flag, enum spdk_opal_locking_range locking_range,
			      const char *passwd);
/* [한국어]
 * spdk_opal_cmd_lock_unlock - 특정 user/range를 lock/unlock 상태로 전환
 *
 * @dev: 활성 Opal 핸들.
 * @user: 명령을 수행할 권한자(Admin1 또는 User1~9). 해당 user는 enable_user로 활성화 +
 *        add_user_to_locking_range로 해당 range의 ACE에 추가되어 있어야 함.
 * @flag: OPAL_READONLY / OPAL_RWLOCK(완전잠금) / OPAL_READWRITE(완전 unlock).
 * @locking_range: 대상 range (global 또는 1~10).
 * @passwd: user의 PIN.
 * @return: 0 성공. 권한자 미인증 시 TCG ACCESS_DENIED, range 미정의 시 INVALID_PARAMETER.
 *
 * 동기/배경: 가장 자주 호출되는 API — 부팅 시 OS가 unlock, 셧다운/대기 진입 시 lock.
 *   range 단위로 read/write를 독립적으로 제어할 수 있는 것이 SED의 핵심 가치.
 * 내부 동작: Locking SP에서 user 권한자로 StartSession → Set 메서드로
 *   LockingRange[range].ReadLocked/WriteLocked 비트 갱신 → EndSession.
 *   비트 매핑: OPAL_READONLY=(R=0,W=1), OPAL_RWLOCK=(R=1,W=1), OPAL_READWRITE=(R=0,W=0).
 * 실행 컨텍스트: 동기. 호스트 유저스페이스. 컨트롤러 소유 SPDK 스레드.
 *   부팅 시점에는 SPDK가 아직 초기화 안 됐을 수 있으므로 nvme-cli 등 외부 도구로 호출되기도 함.
 * 동시성: 동일 dev에 대해 다른 Opal 명령과 동시 수행 금지(SP 단일 세션).
 *   서로 다른 SSD에 대한 호출은 독립이므로 병렬 가능.
 * 에러 경로: 미인증 PIN → ACCESS_DENIED. range 미정의(setup 안 된 1~10) → INVALID_PARAMETER.
 *   user가 ACE에 추가 안 된 경우 → ACCESS_DENIED. 호출자는 음수 반환을 보고
 *   사용자에게 PIN/권한 설정을 안내해야 함.
 * 후속 영향: unlock 후 일반 NVMe I/O가 해당 LBA에 대해 가능. lock 후 read/write IO가
 *   LBA_OUT_OF_RANGE 또는 ACCESS_DENIED로 실패.
 * 호출자: OS 부트로더, sd_unlock 도구, nvme-cli, bdev_nvme RPC.
 * 호출 대상: opal_lock_unlock → security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_lock_unlock → opal_lock_unlock → security_send/receive
 */
int spdk_opal_cmd_setup_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user,
				      enum spdk_opal_locking_range locking_range_id, uint64_t range_start,
				      uint64_t range_length, const char *passwd);
/* [한국어]
 * spdk_opal_cmd_setup_locking_range - locking range의 LBA 영역과 lock policy 정의
 *
 * @dev: 활성 Opal 핸들.
 * @user: 일반적으로 OPAL_ADMIN1(range 정의는 관리자 권한).
 * @locking_range_id: 정의할 range 번호 (1~10). global range(0)는 디바이스 전체로 고정이라
 *   여기서 정의 불가.
 * @range_start: range 시작 LBA. geo.AlignmentGranularity 배수여야 함.
 * @range_length: range 길이 (LBA 개수). 0이면 range 비활성화 의미.
 * @passwd: user PIN(보통 Admin1 PIN).
 * @return: 0 성공, 음수 에러(INVALID_PARAMETER가 가장 흔함 — alignment/한도 위반).
 *
 * 동기/배경: SED의 다중 range 기능을 사용하려면 각 range의 LBA 영역과 잠금 정책을
 *   미리 정의해야 한다. 정의된 후에야 lock_unlock으로 잠금 상태를 토글할 수 있음.
 * 동작: 새 range를 정의하거나 기존 range의 영역/정책을 수정.
 *   내부적으로 LockingRange[id].RangeStart, RangeLength, ReadLockEnabled,
 *   WriteLockEnabled를 Set 메서드로 갱신.
 *   활성화된 range만 lock_unlock으로 잠금 변경 가능.
 * 실행 컨텍스트: 동기. 호스트 유저스페이스. 컨트롤러 소유 SPDK 스레드.
 * 동시성: 다른 Opal 명령과 직렬화 필요(SP 단일 세션). 한 range가 진행 중인 I/O를
 *   포함할 때 영역 변경하면 진행 중 I/O가 잠금 상태에 따라 실패할 수 있음 — 사전에 quiesce 권장.
 * 에러 경로: alignment 미준수 → INVALID_PARAMETER. range 한도 초과 → INVALID_PARAMETER.
 *   PIN 불일치 → ACCESS_DENIED. 다른 range와 LBA 영역이 겹치면 RangeCrossing
 *   feature 비활성 SSD는 INVALID_PARAMETER.
 * 호출자: 사용자 셋업 코드(activate_locking_sp 직후).
 * 호출 대상: opal_setup_range → security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_setup_locking_range → opal_setup_range → security_send/receive
 */

int spdk_opal_cmd_get_max_ranges(struct spdk_opal_dev *dev, const char *passwd);
/* [한국어]
 * spdk_opal_cmd_get_max_ranges - SSD가 지원하는 최대 locking range 개수 조회
 *
 * @dev: 활성 Opal 핸들.
 * @passwd: Admin1 PIN.
 * @return: 0 성공 (결과는 dev 내부 캐시에 저장 — 별도 getter 사용),
 *          음수 에러.
 *
 * 동기/배경: 호스트가 setup_locking_range로 정의 가능한 range 수를 미리 알아야 정책
 *   결정이 가능. v200 feature의 num_locking_ranges와 다를 수 있어(펌웨어 동적 한도)
 *   런타임 조회가 필요.
 * 동작: Locking SP의 LockingInfo 객체에서 MaxRanges 컬럼을 Get으로 읽어 dev 내부 캐시 갱신.
 *   표준 Opal v2.00은 최소 8개 보장.
 * 실행 컨텍스트: 동기. 호스트 유저스페이스. 컨트롤러 소유 SPDK 스레드.
 * 동시성: 다른 Opal 명령과 직렬화. read-only 조회지만 SP 세션을 점유하므로 단일 진행.
 * 에러 경로: PIN 불일치 → ACCESS_DENIED. Locking SP 미활성 → 메서드 거부.
 * 호출자: 사용자 셋업 도구가 setup_locking_range 호출 전에.
 * 호출 대상: opal_get_max_ranges → security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_get_max_ranges → opal_get_max_ranges → security_send/receive
 */
int spdk_opal_cmd_get_locking_range_info(struct spdk_opal_dev *dev, const char *passwd,
		enum spdk_opal_user user_id,
		enum spdk_opal_locking_range locking_range_id);
/* [한국어]
 * spdk_opal_cmd_get_locking_range_info - 특정 range의 LBA/잠금 상태 조회
 *
 * @dev: 활성 Opal 핸들.
 * @passwd: user PIN.
 * @user_id: 조회 권한자(Admin1 또는 해당 range의 ACE에 추가된 User).
 * @locking_range_id: 조회 대상 range.
 * @return: 0 성공 (결과는 dev 내부 spdk_opal_locking_range_info 슬롯에 저장 —
 *          spdk_opal_get_locking_range_info()로 접근), 음수 에러.
 *
 * 동기/배경: 사용자가 현재 range의 LBA/잠금 상태를 확인해야 하는 경우(예: 부팅 시
 *   이미 unlock인지, lock 정책이 켜져 있는지) 사용. 결과를 dev 내부 캐시에 두어
 *   이후 spdk_opal_get_locking_range_info()가 동기 getter로 반환.
 * 동작: Get 메서드로 LockingRange 객체의 RangeStart/RangeLength/Read/WriteLockEnabled/
 *   Read/WriteLocked 컬럼을 읽어 내부 캐시에 저장.
 * 실행 컨텍스트: 동기. 호스트 유저스페이스. 컨트롤러 소유 SPDK 스레드.
 * 동시성: SP 단일 세션 — 다른 Opal 명령과 직렬화.
 * 에러 경로: PIN 불일치 → ACCESS_DENIED. range 미정의 → INVALID_PARAMETER.
 *   user가 해당 range 조회 권한 없음 → ACCESS_DENIED.
 *   호출자가 비-0 반환 시 캐시 슬롯은 stale일 수 있어 사용 금지.
 * 호출자: 사용자 모니터링 도구, GUI 정보 페인.
 * 호출 대상: opal_get_range_info → security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_get_locking_range_info → opal_get_range_info → security_send/receive
 */
int spdk_opal_cmd_enable_user(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
			      const char *passwd);
/* [한국어]
 * spdk_opal_cmd_enable_user - 사용자 권한자(User1~9)를 활성화
 *
 * @dev: 활성 Opal 핸들.
 * @user_id: 활성화할 user (OPAL_USER1~9).
 * @passwd: Admin1 PIN — Admin1만 사용자 활성화 가능.
 * @return: 0 성공, 음수 에러.
 *
 * 동기/배경: Locking SP의 User1~9는 출하 시 비활성 상태. 멀티 테넌트 환경에서
 *   사용자별 PIN/range 권한을 부여하려면 먼저 활성화가 필요.
 * 동작: Authority 테이블의 user.Enabled 컬럼을 true로 Set.
 * 후속 단계: 활성화 후 set_new_passwd로 PIN을 부여하고, add_user_to_locking_range로
 *   ACE에 추가하면 lock_unlock 명령을 수행할 수 있게 됨.
 * 실행 컨텍스트: 동기. 호스트 유저스페이스. 컨트롤러 소유 SPDK 스레드.
 * 동시성: 다른 Opal 명령과 직렬화.
 * 에러 경로: PIN 불일치 → ACCESS_DENIED. user_id가 펌웨어 한도(v200.num_users)를 초과 →
 *   INVALID_PARAMETER. 이미 활성화된 user에 대한 호출은 No-op로 성공 반환.
 * 호출자: 사용자 셋업 코드(activate_locking_sp 후).
 * 호출 대상: opal_enable_user → security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_enable_user → opal_enable_user → security_send/receive
 */
int spdk_opal_cmd_add_user_to_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
		enum spdk_opal_locking_range locking_range_id,
		enum spdk_opal_lock_state lock_flag, const char *passwd);
/* [한국어]
 * spdk_opal_cmd_add_user_to_locking_range - 특정 user를 range의 ACE에 추가
 *
 * @dev: 활성 Opal 핸들.
 * @user_id: 추가할 user (이미 enable_user로 활성화 + PIN 부여 완료 상태여야 함).
 * @locking_range_id: 대상 range.
 * @lock_flag: 어떤 잠금 동작(SetLockingRange_RDLocked/_WRLocked 메서드)에 권한을 부여할지.
 * @passwd: Admin1 PIN.
 * @return: 0 성공, 음수 에러.
 *
 * 동기/배경: TCG SED는 권한 부여 모델로 ACE(Access Control Element) 테이블의
 *   BooleanACE 행을 사용한다. 어떤 user가 어떤 range를 lock/unlock할 수 있는지
 *   여기서 결정. 활성화된 user라도 ACE에 추가되지 않으면 lock_unlock이 ACCESS_DENIED.
 * 동작: ACE 테이블에 BooleanACE 행을 추가/수정해 user가 lock_unlock 메서드를 호출할
 *   수 있도록 권한 부여.
 * 실행 컨텍스트: 동기. 호스트 유저스페이스. 컨트롤러 소유 SPDK 스레드.
 * 동시성: 다른 Opal 명령과 직렬화.
 * 에러 경로: user 미활성 → INVALID_PARAMETER. range 미정의 → INVALID_PARAMETER.
 *   PIN 불일치 → ACCESS_DENIED. lock_flag 부적절(예: 알 수 없는 비트) → INVALID_PARAMETER.
 * 호출자: 사용자 셋업 코드(enable_user/set_new_passwd 후).
 * 호출 대상: opal_add_user_to_lr → security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_add_user_to_locking_range → opal_add_user_to_lr → security_send/receive
 */
int spdk_opal_cmd_set_new_passwd(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
				 const char *new_passwd, const char *old_passwd, bool new_user);
/* [한국어]
 * spdk_opal_cmd_set_new_passwd - 권한자 PIN 변경 또는 신규 설정
 *
 * @dev: 활성 Opal 핸들.
 * @user_id: 대상 권한자.
 * @new_passwd: 새 PIN(1~32바이트 권장).
 * @old_passwd: 기존 PIN. new_user=false이면 user_id 자신의 PIN, true면 Admin1 PIN.
 * @new_user: true이면 새로 활성화한 사용자에게 첫 PIN 설정 (Admin1이 대신 설정),
 *            false이면 기존 사용자가 자신의 PIN 변경.
 * @return: 0 성공, 음수 에러.
 *
 * 동기/배경: TCG는 PIN 자체가 인증 자격이므로 변경/초기 설정 흐름이 분리된다.
 *   new_user=true 모드는 "Admin1이 신규 사용자에게 초기 PIN 부여" 케이스 — 사용자
 *   자신의 old PIN이 아직 없는 상태이므로 Admin1 PIN으로 인증.
 * 동작: C_PIN 테이블의 user.PIN 컬럼을 Set 메서드로 갱신. PIN은 평문이 와이어로
 *   흐르지만 Security Send/Receive(0x81/0x82)는 PCIe TLP에서 호스트-디바이스 간만
 *   전송되며 외부에 노출되지 않음(스니핑 위협은 PCIe analyzer 부착 시 한정).
 * 실행 컨텍스트: 동기. 호스트 유저스페이스. 컨트롤러 소유 SPDK 스레드.
 * 동시성: 다른 Opal 명령과 직렬화. 같은 user에 대한 동시 변경은 마지막 호출이 이김
 *   (펌웨어 직렬화).
 * 에러 경로: old_passwd 불일치 → ACCESS_DENIED. new_passwd가 너무 길거나 NULL →
 *   INVALID_PARAMETER. new_user=true인데 user_id가 이미 PIN을 가짐 → 펌웨어 정책에
 *   따라 ACCESS_DENIED 또는 그대로 덮어쓰기.
 * 호출자: 사용자 PIN 변경 도구, 셋업 스크립트.
 * 호출 대상: opal_set_new_pin → security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_set_new_passwd → opal_set_new_pin → security_send/receive
 */

int spdk_opal_cmd_erase_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
				      enum spdk_opal_locking_range locking_range_id, const char *password);
/* [한국어]
 * spdk_opal_cmd_erase_locking_range - 특정 range의 데이터를 cryptographic erase
 *
 * @dev: 활성 Opal 핸들.
 * @user_id: 권한자(보통 Admin1).
 * @locking_range_id: 삭제할 range.
 * @password: user_id PIN.
 * @return: 0 성공, 음수 에러.
 *
 * 동기/배경: 사용자가 range의 데이터를 안전하게 폐기하고 다른 용도로 재사용하려 할 때
 *   필요. 키만 폐기하므로 NAND write가 불필요해 SSD 수명 영향이 거의 없으며
 *   완료 시간도 매우 짧음(보통 수백 ms).
 * 동작: GenKey 메서드로 range의 미디어 암호화 키(MEK; Media Encryption Key)를 새로 생성 —
 *   기존 데이터를 암호학적으로 복구 불가능하게 만듦. 실제 NAND 데이터 자체는 그대로 두고
 *   키만 폐기. range 정의(LBA 범위)는 보존되며 재사용 가능.
 * 실행 컨텍스트: 동기. 호스트 유저스페이스. 컨트롤러 소유 SPDK 스레드.
 * 동시성: 다른 Opal 명령과 직렬화. range를 사용하는 진행 중 일반 NVMe I/O가
 *   있다면 사전에 quiesce해야 안전 — 펌웨어가 진행 중 I/O를 abort할 가능성 있음.
 * 에러 경로: PIN 불일치 → ACCESS_DENIED. range 미정의 → INVALID_PARAMETER.
 *   user가 GenKey 권한 ACE에 없으면 ACCESS_DENIED.
 * 후속 영향: 이 호출 후 해당 range의 모든 LBA를 read하면 0 또는 임의의 패턴(키 폐기 후
 *   복호화 결과)이 반환됨 — 결정적 응답을 원하면 이후 비밀데이터 0 write가 필요.
 * 호출자: 사용자 데이터 정리 도구, secure-decommission 흐름.
 * 호출 대상: opal_erase_lr → security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_erase_locking_range → opal_erase_lr → security_send/receive
 */

int spdk_opal_cmd_secure_erase_locking_range(struct spdk_opal_dev *dev, enum spdk_opal_user user_id,
		enum spdk_opal_locking_range locking_range_id, const char *password);
/* [한국어]
 * spdk_opal_cmd_secure_erase_locking_range - secure erase 변형 (range + 사용자 정보 삭제)
 *
 * @dev: 활성 Opal 핸들.
 * @user_id: 권한자.
 * @locking_range_id: 대상 range.
 * @password: user_id PIN.
 * @return: 0 성공, 음수 에러.
 *
 * 동기/배경: 단순 데이터 삭제(erase_locking_range)에서 더 나아가 range에 결합된
 *   사용자 권한 정보까지 한 번에 정리하고 싶을 때 사용. 멀티 테넌트 환경에서 한
 *   사용자가 떠날 때 그 사용자의 데이터+권한을 함께 회수하는 흐름에 적합.
 * 동작: EraseMethod 메서드 호출 — range 데이터 키 폐기에 더해 해당 range에 연결된
 *   사용자 권한자 PIN과 ACE 권한도 함께 초기화. 즉 erase_locking_range보다
 *   더 광범위한 정리.
 * 실행 컨텍스트: 동기. 호스트 유저스페이스. 컨트롤러 소유 SPDK 스레드.
 * 동시성: 다른 Opal 명령과 직렬화. 사용자 권한이 변경되므로 진행 중 다른 user의
 *   세션이 있다면 EndSession을 우선 강제할 수 있음(펌웨어 정책).
 * 에러 경로: PIN 불일치 → ACCESS_DENIED. user_id가 EraseMethod 권한 ACE에 없으면
 *   ACCESS_DENIED. range 미정의 → INVALID_PARAMETER.
 * 후속 영향: 이 호출 후 해당 range를 다시 사용하려면 setup_locking_range,
 *   add_user_to_locking_range 등을 다시 수행해야 함.
 * 호출자: 멀티 테넌트 관리 도구, 사용자 오프보딩 스크립트.
 * 호출 대상: opal_secure_erase_lr → security_send/receive.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_cmd_secure_erase_locking_range → opal_secure_erase_lr → security_send/receive
 */

struct spdk_opal_locking_range_info *spdk_opal_get_locking_range_info(struct spdk_opal_dev *dev,
		enum spdk_opal_locking_range id);
/* [한국어]
 * spdk_opal_get_locking_range_info - 캐시된 locking range info 조회
 *
 * @dev: 활성 Opal 핸들.
 * @id: 조회할 range.
 * @return: 캐시된 spdk_opal_locking_range_info 포인터.
 *          spdk_opal_cmd_get_locking_range_info를 먼저 호출하지 않았으면 NULL.
 *
 * 동기/배경: spdk_opal_cmd_get_locking_range_info()가 동기 NVMe 왕복으로 SSD에서 읽어와
 *   dev 내부 슬롯에 캐시한 결과를 사용자 코드에 노출하는 단순 접근자. NVMe 왕복 없이
 *   즉시 값을 사용하고 싶을 때 호출.
 * 동작: 단순 getter — dev->locking_ranges[id]의 주소를 반환. 네트워크 왕복 없음.
 * 실행 컨텍스트: 임의의 SPDK 스레드(read-only 캐시).
 * 동시성: 캐시는 cmd_get_locking_range_info 호출 후 read-only — 다중 스레드 동시 read 안전.
 *   단, free_locking_range_info와 동시 호출하면 use-after-free 위험 — 호출자가 lifetime 보장.
 * 에러 경로: id가 범위 밖이거나 캐시가 비어 있으면 NULL 반환.
 * 반환 포인터의 수명: spdk_opal_free_locking_range_info 호출 전까지 또는 dev이
 *   destruct되기 전까지 유효.
 * 호출자: 사용자 모니터링/표시 코드.
 * 호출 대상: 없음(단순 getter).
 *
 * 호출 체인:
 *   사용자 → spdk_opal_get_locking_range_info → return &dev->locking_ranges[id]
 */
void spdk_opal_free_locking_range_info(struct spdk_opal_dev *dev, enum spdk_opal_locking_range id);
/* [한국어]
 * spdk_opal_free_locking_range_info - 캐시된 range info를 해제(슬롯 비우기)
 *
 * @dev: 활성 Opal 핸들.
 * @id: 해제할 range.
 * @return: void — 슬롯이 이미 비어 있어도 무시(no-op).
 *
 * 동기/배경: 사용자가 spdk_opal_get_locking_range_info로 받은 포인터를 더 이상
 *   사용하지 않을 때, 메모리 사용을 줄이거나 캐시를 무효화하기 위해 호출.
 *   destruct 시점에 자동으로 호출되므로 명시적 free는 선택사항.
 * 동작: 내부 캐시 슬롯의 동적 할당 영역(있다면)을 해제하고 슬롯을 비활성 상태로 표시.
 *   이후 spdk_opal_get_locking_range_info(dev, id)는 NULL 반환.
 * 실행 컨텍스트: 임의의 SPDK 스레드. 단, dev 단위 동시성 규칙은 따라야 함.
 * 동시성: 호출 중 다른 스레드가 spdk_opal_get_locking_range_info(dev, id)를 호출하면
 *   use-after-free 가능 — 호출자가 사전에 read 작업을 quiesce해야 함.
 * 에러 경로: id 범위 밖이면 무시(no-op).
 * 호출자: 사용자 정리 코드, 또는 destruct 내부.
 * 호출 대상: free 내부 버퍼 + 슬롯 메타데이터 reset.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_free_locking_range_info → free 내부 버퍼
 */

#ifdef __cplusplus
}
/* [한국어] extern "C" 종료. */
#endif

#endif
/* [한국어] SPDK_OPAL_H 가드 종료. */
