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
	/* [한국어] TPer Feature(0x0001) — Trusted Peripheral 기본 능력 비트맵.
	 * Sync/Async/AckNak/Buffer Mgmt/Streaming/ComID Mgmt 지원 여부 포함. */

	struct spdk_opal_d0_locking_feat locking;
	/* [한국어] Locking Feature(0x0002) — Locking SP 활성화/잠금 상태/MBR 활성화 비트.
	 * LockingEnabled 비트가 0이면 take_ownership/activate_locking_sp가 선행되어야 함. */

	struct spdk_opal_d0_single_user_mode_feat single_user;
	/* [한국어] Single User Mode Feature(0x0201) — 1 사용자 = 1 range로 단순화한 모드.
	 * SUM 활성 여부, 자동 lock 정책, 잠금 범위 수 등 포함. */

	struct spdk_opal_d0_geo_feat geo;
	/* [한국어] Geometry Feature(0x0003) — 논리 블록 크기, alignment granularity.
	 * Opal range 시작/길이 LBA가 alignment에 맞아야 명령이 수락됨. */

	struct spdk_opal_d0_datastore_feat datastore;
	/* [한국어] DataStore Table Feature(0x0202) — 호스트가 임의 데이터를 저장할 수 있는
	 * 공간(테이블 크기/개수). nvme-cli의 datastore put/get에 사용. */

	struct spdk_opal_d0_v100_feat v100;
	/* [한국어] Opal SSC v1.00 Feature(0x0200) — base comID, 사용자/locking range 한도.
	 * 펌웨어가 v1.00 모드만 지원하면 v2.00 비기능적 비트는 의미 없음. */

	struct spdk_opal_d0_v200_feat v200;
	/* [한국어] Opal SSC v2.00 Feature(0x0203) — base comID, 사용자/admin 수, RangeCrossing.
	 * SPDK는 v2.00 호환 SED를 주 타깃으로 하며, 사용자 ID 매핑(OPAL_USER1..9)이 여기 정의됨. */
};

enum spdk_opal_lock_state {
	/* [한국어] spdk_opal_cmd_lock_unlock의 flag 인자로 사용되는 잠금 상태 enum.
	 * TCG SWG 메서드 SetLocking_*에 들어가는 ReadLocked/WriteLocked 비트와 매핑된다.
	 * 설정자: API 호출자. 읽는 자: nvme_opal.c가 토큰 빌드 시 비트로 변환. */

	OPAL_READONLY	= 0x01,
	/* [한국어] WriteLocked=true, ReadLocked=false → 읽기만 허용, 쓰기 차단.
	 * 부팅 후 OS가 read-only로 마운트할 때 사용. */

	OPAL_RWLOCK		= 0x02,
	/* [한국어] WriteLocked=true, ReadLocked=true → 모든 I/O 차단(완전 잠금).
	 * 시스템 셧다운 시 또는 분실 보호 모드. */

	OPAL_READWRITE	= 0x04,
	/* [한국어] WriteLocked=false, ReadLocked=false → 정상 읽기/쓰기 허용 (잠금 해제).
	 * 사용자가 PIN으로 인증 후 OS 부팅 시 unlock에 사용. */
};

enum spdk_opal_user {
	/* [한국어] Opal SP의 권한자(Authority) 식별자 — Locking SP 내 user/admin 슬롯.
	 * TCG Opal v2.00은 Admin 1개 + User 1~9 (총 10명)를 표준 정의.
	 * 설정자: API 호출자. 읽는 자: nvme_opal.c가 권한자 UID(8바이트)로 변환해
	 *   StartSession 메서드의 HostSigningAuthority 파라미터에 인코딩. */

	OPAL_ADMIN1 = 0x0,
	/* [한국어] Locking SP Admin1 — Locking SP의 슈퍼유저. take_ownership 후 첫 PIN 보유자.
	 * 사용자 추가/삭제, locking range 정의, secure erase 등 모든 관리 작업 가능. */

	OPAL_USER1 = 0x01,
	/* [한국어] User1 — 일반 사용자 권한자. enable_user로 활성화 후 PIN 설정 필요.
	 * add_user_to_locking_range로 특정 range의 BooleanACE에 추가되어야 lock/unlock 가능. */

	OPAL_USER2 = 0x02,
	/* [한국어] User2 — 동일 패턴, 보통 멀티 테넌트 시 두 번째 사용자 슬롯. */

	OPAL_USER3 = 0x03,
	/* [한국어] User3. */

	OPAL_USER4 = 0x04,
	/* [한국어] User4. */

	OPAL_USER5 = 0x05,
	/* [한국어] User5. */

	OPAL_USER6 = 0x06,
	/* [한국어] User6. */

	OPAL_USER7 = 0x07,
	/* [한국어] User7. */

	OPAL_USER8 = 0x08,
	/* [한국어] User8. */

	OPAL_USER9 = 0x09,
	/* [한국어] User9 — TCG Opal v2.00이 보장하는 마지막 표준 사용자 슬롯.
	 * 펌웨어가 v200.num_users로 보고하는 한도 내에서 사용. */
};

enum spdk_opal_locking_range {
	/* [한국어] Opal locking range 식별자 — 디스크의 LBA 영역을 나누어 각각 다른 잠금 정책 적용.
	 * Global range는 항상 존재하며 디스크 전체를 커버, 1~10번 range는 사용자가 정의.
	 * 설정자: API 호출자. 읽는 자: nvme_opal.c가 LockingRange UID(LR_global, LR_1..10)로 변환. */

	OPAL_LOCKING_RANGE_GLOBAL = 0x0,
	/* [한국어] Global locking range — 디스크 전체. 다른 range가 정의되지 않은 모든 LBA를 커버.
	 * 단순한 Full Disk Encryption 모드에서는 이 range만 사용. */

	OPAL_LOCKING_RANGE_1,
	/* [한국어] Locking Range 1 — setup_locking_range로 LBA 시작/길이를 정의해야 활성. */

	OPAL_LOCKING_RANGE_2,
	/* [한국어] Range 2. */

	OPAL_LOCKING_RANGE_3,
	/* [한국어] Range 3. */

	OPAL_LOCKING_RANGE_4,
	/* [한국어] Range 4. */

	OPAL_LOCKING_RANGE_5,
	/* [한국어] Range 5. */

	OPAL_LOCKING_RANGE_6,
	/* [한국어] Range 6. */

	OPAL_LOCKING_RANGE_7,
	/* [한국어] Range 7. */

	OPAL_LOCKING_RANGE_8,
	/* [한국어] Range 8. */

	OPAL_LOCKING_RANGE_9,
	/* [한국어] Range 9. */

	OPAL_LOCKING_RANGE_10,
	/* [한국어] Range 10 — 표준 Opal v2.00 기본 한도. SSD가 지원하는 실제 한도는
	 * spdk_opal_cmd_get_max_ranges로 조회 가능 (펌웨어/제조사에 따라 다름). */
};

struct spdk_opal_locking_range_info {
	/* [한국어] 단일 locking range의 LBA 정의와 현재 잠금 상태를 캐시하는 구조체.
	 * spdk_opal_cmd_get_locking_range_info() 결과를 보관하며,
	 * spdk_opal_get_locking_range_info()로 조회 / spdk_opal_free_locking_range_info()로 해제.
	 * 설정자: nvme_opal.c가 SSD 응답 파싱 후 채움.
	 * 읽는 자: 사용자가 직접 필드 접근.
	 * 동기화: 사용자가 free 호출 시 nvme_opal.c가 내부 버퍼를 NULL로 표시 — 동시 접근 금지. */

	uint8_t locking_range_id;
	/* [한국어] 어느 range를 가리키는지 (0=global, 1~10).
	 * 설정자: 응답 파서. 읽는 자: 사용자. */

	uint8_t _padding[7];
	/* [한국어] 64비트 정렬 패딩 — 다음 uint64_t 필드 alignment 맞춤.
	 * 의도적 패딩으로 호스트 시스템 ABI에 무관하게 일정 레이아웃 유지. */

	uint64_t range_start;
	/* [한국어] range의 시작 LBA. global range는 항상 0.
	 * 단위는 논리 블록(geo.LogicalBlockSize). alignment granularity 배수여야 함. */

	uint64_t range_length;
	/* [한국어] range의 길이(LBA 개수). 0이면 미정의 range. */

	bool read_lock_enabled;
	/* [한국어] 이 range에 read lock 정책이 활성화되어 있는지 (true면 ReadLocked 비트 의미 있음).
	 * 설정자: SSD 펌웨어. 읽는 자: 호스트가 잠금 해제 절차 분기 결정. */

	bool write_lock_enabled;
	/* [한국어] write lock 정책 활성화 여부. read_lock_enabled와 독립적. */

	bool read_locked;
	/* [한국어] 현재 read가 잠겨 있는지 (true=차단, false=허용).
	 * read_lock_enabled가 false면 이 비트는 무의미. */

	bool write_locked;
	/* [한국어] 현재 write가 잠겨 있는지. */
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
 * 호출 시 내부적으로:
 *   1) calloc으로 spdk_opal_dev 할당 + 컨트롤러 포인터 저장
 *   2) Level 0 Discovery 발행 (Security Receive opcode 0x82, ProtocolID=0x01, ComID=0x0001)
 *   3) 응답 파싱하여 d0_features_info 7개 디스크립터 캐시
 *   4) v200.base_comid를 추출해 이후 모든 SWG 명령에 사용
 * 호출 컨텍스트: SPDK 어플리케이션 스레드. Admin qpair 폴링이 동기적으로 일어나므로
 * 컨트롤러 처리 스레드에서 호출되어야 함 (NVMe 스레드 어피니티 규칙).
 *
 * 호출 체인:
 *   사용자 → spdk_opal_dev_construct → opal_init/opal_discovery0 → spdk_nvme_ctrlr_security_receive
 */
void spdk_opal_dev_destruct(struct spdk_opal_dev *dev);
/* [한국어]
 * spdk_opal_dev_destruct - Opal 핸들 해제
 *
 * @dev: spdk_opal_dev_construct가 반환한 핸들. NULL은 무시.
 * @return: void.
 *
 * 활성 세션이 있다면 EndSession을 보내려 시도하고, 실패해도 메모리는 해제.
 * 핸들 해제 후 컨트롤러는 정상 동작하지만 Opal 명령은 더 이상 발행 불가.
 * 호출 컨텍스트: 사용자 정리 단계, spdk_nvme_detach 직전.
 *
 * 호출 체인:
 *   사용자 → spdk_opal_dev_destruct → free(dev)
 */

struct spdk_opal_d0_features_info *spdk_opal_get_d0_features_info(struct spdk_opal_dev *dev);
/* [한국어]
 * spdk_opal_get_d0_features_info - 캐시된 Level 0 Discovery 결과 반환
 *
 * @dev: 활성 Opal 핸들.
 * @return: dev 내부에 저장된 d0_features_info 포인터 (사용자는 read-only로 사용).
 *          dev이 NULL이면 NULL.
 *
 * Discovery는 construct 시 한 번만 수행하므로 이 함수는 단순 getter (네트워크 왕복 없음).
 * 반환 포인터의 수명은 dev이 살아 있는 동안 유효.
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
 * @return: 0 성공, 음수 errno 또는 TCG 에러 코드.
 *
 * Opal 활성화의 첫 단계 — TCG SSC §3.1.1.4. 내부 동작:
 *   1) Admin SP에서 MSID 권한자로 StartSession (현재 PIN = MSID, 공장 디폴트)
 *   2) Get MSID 메서드로 32바이트 MSID PIN 회수 (펌웨어가 PSID 라벨 등으로 노출)
 *   3) Set 메서드로 SID PIN을 new_passwd로 갱신
 *   4) EndSession
 * 이 호출 후 SSD는 사용자가 PIN을 소유한 상태가 되며, 이후 모든 명령은 new_passwd 필요.
 * 호출 컨텍스트: 동기 — Admin qpair 폴링이 완료될 때까지 블로킹.
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
int spdk_opal_cmd_revert_tper(struct spdk_opal_dev *dev, const char *passwd);
/* [한국어]
 * spdk_opal_cmd_revert_tper - TPer 전체를 공장 초기 상태로 되돌림
 *
 * @dev: 활성 Opal 핸들.
 * @passwd: SID 권한자 PIN.
 * @return: 0 성공, 음수 에러 코드.
 *
 * RevertSP/Revert 메서드를 호출해 모든 PIN, locking range, 사용자 권한자, 데이터스토어를
 * 초기화하고 모든 사용자 데이터 영역에 cryptographic erase(미디어 키 폐기)를 수행한다.
 * 데이터는 즉시 복구 불가 — 매우 파괴적인 명령.
 * 동작 후 SSD는 take_ownership 이전 상태로 돌아감 (MSID PIN 활성).
 * 위 영문 주석대로 동기 호출 — 응답 도착까지 함수가 블로킹.
 * 호출 컨텍스트: 사용자 명시적 erase 의도 시.
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
 * @return: 0 성공, 음수 에러 코드.
 *
 * Locking SP는 생산 직후 Manufactured-Inactive 상태이며, 이를 Manufactured로 전이해야
 * locking range 사용/사용자 추가가 가능해진다 (TCG SSC §3.1.1.4).
 * Activate 메서드 호출 후 Locking SP의 Admin1 PIN이 자동으로 SID PIN과 동일하게 설정됨.
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
 * 가장 자주 호출되는 API — 부팅 시 OS가 unlock, 셧다운 시 lock.
 * 내부 동작: Locking SP에서 user 권한자로 StartSession → Set 메서드로
 * LockingRange[range].ReadLocked/WriteLocked 비트 갱신 → EndSession.
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
 * @user: 일반적으로 OPAL_ADMIN1.
 * @locking_range_id: 정의할 range 번호 (1~10).
 * @range_start: range 시작 LBA. geo.AlignmentGranularity 배수여야 함.
 * @range_length: range 길이 (LBA 개수).
 * @passwd: user PIN.
 * @return: 0 성공, 음수 에러.
 *
 * 새 range를 정의하거나 기존 range의 영역/정책을 수정.
 * 내부적으로 LockingRange[id].RangeStart, RangeLength, ReadLockEnabled,
 * WriteLockEnabled를 Set 메서드로 갱신.
 * 활성화된 range만 lock_unlock으로 잠금 변경 가능.
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
 * MaxRanges 메서드를 호출해 SSD 펌웨어가 노출하는 range 한도(global 제외)를 알아냄.
 * 표준 Opal v2.00은 최소 8개 보장.
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
 * @user_id: 조회 권한자.
 * @locking_range_id: 조회 대상 range.
 * @return: 0 성공 (결과는 dev 내부 spdk_opal_locking_range_info 슬롯에 저장 —
 *          spdk_opal_get_locking_range_info()로 접근), 음수 에러.
 *
 * Get 메서드로 LockingRange 객체의 RangeStart/RangeLength/Read/WriteLockEnabled/
 * Read/WriteLocked 컬럼을 읽어 내부 캐시에 저장.
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
 * @passwd: Admin1 PIN.
 * @return: 0 성공, 음수 에러.
 *
 * Authority 테이블의 user.Enabled 컬럼을 true로 Set.
 * 활성화 후 set_new_passwd로 PIN을 부여하고, add_user_to_locking_range로 ACE에 추가하면
 * lock_unlock 명령을 수행할 수 있게 됨.
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
 * @user_id: 추가할 user.
 * @locking_range_id: 대상 range.
 * @lock_flag: 어떤 잠금 동작(SetLockingRange_RDLocked/_WRLocked 메서드)에 권한을 부여할지.
 * @passwd: Admin1 PIN.
 * @return: 0 성공, 음수 에러.
 *
 * ACE(Access Control Element) 테이블에 BooleanACE 행을 추가/수정해 user가 lock_unlock
 * 메서드를 호출할 수 있도록 권한 부여.
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
 * @new_passwd: 새 PIN.
 * @old_passwd: 기존 PIN. new_user=false이면 user_id 자신의 PIN, true면 Admin1 PIN.
 * @new_user: true이면 새로 활성화한 사용자에게 첫 PIN 설정 (Admin1이 대신 설정),
 *            false이면 기존 사용자가 자신의 PIN 변경.
 * @return: 0 성공, 음수 에러.
 *
 * C_PIN 테이블의 user.PIN 컬럼을 Set 메서드로 갱신. PIN은 평문이 와이어로 흐르지만
 * Security Send/Receive(0x81/0x82)는 PCIe TLP에서 호스트-디바이스 간만 전송되며
 * 외부에 노출되지 않음.
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
 * GenKey 메서드로 range의 미디어 암호화 키(MEK)를 새로 생성 — 기존 데이터를
 * 암호학적으로 복구 불가능하게 만듦. 실제 NAND 데이터 자체는 그대로 두고 키만 폐기.
 * range 정의(LBA 범위)는 보존되며 재사용 가능.
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
 * EraseMethod 메서드 호출 — range 데이터 키 폐기에 더해 해당 range에 연결된
 * 사용자 권한자 PIN과 ACE 권한도 함께 초기화. 즉 erase_locking_range보다
 * 더 광범위한 정리.
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
 * 단순 getter — 네트워크 왕복 없음. 반환 포인터의 수명은
 * spdk_opal_free_locking_range_info 호출 전까지 또는 dev이 살아 있는 동안.
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
 * @return: void.
 *
 * 내부 캐시 슬롯의 동적 할당 영역(있다면)을 해제하고 슬롯을 비활성 상태로 표시.
 * 이후 spdk_opal_get_locking_range_info(dev, id)는 NULL 반환.
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
