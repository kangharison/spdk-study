/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] TCG Opal SED 가상 bdev 공개 헤더 (vbdev_opal.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 NVMe 컨트롤러가 지원하는 TCG(Trusted Computing Group) Opal SED
 * (Self-Encrypting Drive) 기능을 SPDK bdev 추상화로 노출하기 위한 가상 bdev
 * (vbdev_opal)의 공개 API를 선언한다. Opal SED는 드라이브 펌웨어 내부에서 자체
 * 암호화/잠금을 수행하는 표준으로, 호스트는 Locking Range를 정의하고 각 범위를
 * 패스워드로 잠그거나 해제할 수 있다. 이 모듈은 단일 NVMe namespace의 LBA 범위를
 * 하나의 Opal Locking Range에 매핑하여, 그 범위만 노출하는 새로운 bdev를 만든다.
 * 즉 vbdev_opal bdev 하나는 (NVMe ctrlr, namespace, locking_range_id) 삼중조의
 * 인증된 영역을 의미한다. RPC 핸들러(vbdev_opal_rpc.c)와 구현부(vbdev_opal.c)가
 * 이 헤더의 함수 시그니처를 공유한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   외부 RPC 클라이언트 (bdev_opal_create 등)
 *     → vbdev_opal_rpc.c::rpc_bdev_opal_create() 등의 핸들러
 *     → 본 헤더에 선언된 vbdev_opal_*() API
 *     → vbdev_opal.c 내부 구현이 nvme_ctrlr->opal_dev (lib/nvme의 opal 드라이버)에
 *        spdk_opal_cmd_*() 호출을 위임
 *     → 인증 성공 시 spdk_bdev_register()로 부분 LBA 범위만 노출하는 새 bdev 생성
 * 실행 컨텍스트: SPDK app 스레드 (RPC 핸들러는 RPC 서버 스레드에서 호출되며,
 * bdev_register/unregister 같은 코어 작업은 app 스레드에서 동기적으로 수행).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: bdev_nvme.h (struct nvme_ctrlr, nvme_ctrlr_get_by_name 사용),
 *         spdk/bdev_module.h (bdev 등록 API), spdk/opal.h (lib/nvme/opal 드라이버 API).
 * - 의존받음: vbdev_opal_rpc.c (RPC 핸들러), bdev_nvme.c (vbdev_opal과 협력하여
 *   nvme_ctrlr 해제 시점에 vbdev_opal도 정리).
 * - 데이터 흐름: 호스트 사용자가 입력한 패스워드 → Opal 드라이버 → SED 내부 인증 →
 *   인증 성공 시 LBA 범위가 잠금 해제됨. 호스트의 일반 read/write는 평소 NVMe와
 *   동일한 경로(qpair → SQ doorbell)로 흐른다. vbdev_opal은 인증/잠금 게이트만
 *   담당하며 데이터 경로 자체는 가로채지 않는다 (LBA range 보호는 SED 펌웨어가 수행).
 *
 * === 주요 함수/구조체 요약 ===
 * - vbdev_opal_create: NVMe ctrlr/nsid/locking_range_id를 받아 새 vbdev 생성.
 * - vbdev_opal_destruct: 패스워드 인증 후 vbdev unregister 및 잠금 복원.
 * - vbdev_opal_enable_new_user: Admin 인증 후 추가 사용자 슬롯에 권한 부여.
 * - vbdev_opal_set_lock_state: read/write/lock/unlock 등 잠금 상태 변경.
 * - vbdev_opal_get_info_from_bdev: 패스워드 인증 후 Locking Range 메타데이터 조회.
 */

#ifndef SPDK_VBDEV_OPAL_H
#define SPDK_VBDEV_OPAL_H

#include "spdk/bdev_module.h"   /* [한국어] spdk_bdev_register/unregister 등 bdev 모듈 API. */
#include "bdev_nvme.h"           /* [한국어] struct nvme_ctrlr 정의와 nvme_ctrlr_get_by_name 선언. */

/*
 * [한국어]
 * vbdev_opal_create - NVMe namespace의 일부 LBA 범위를 Opal 보호 vbdev로 노출한다.
 *
 * @nvme_ctrlr_name: bdev_nvme RPC로 등록된 NVMe 컨트롤러 이름 (예: "Nvme0").
 * @nsid:            namespace ID (1부터 시작하는 NVMe namespace 번호).
 * @locking_range_id: TCG Opal Locking SP가 관리하는 Locking Range 인덱스 (0~7 등 SED 의존).
 * @range_start:     이 vbdev가 노출할 시작 LBA (NVMe namespace 기준 절대 LBA).
 * @range_length:    노출할 LBA 개수 (블록 수).
 * @password:        Admin SP 인증에 사용할 패스워드 (호스트 사용자가 입력).
 * @return: 0 성공, 음수 errno (잘못된 인자/메모리 부족/Opal 인증 실패 등).
 *
 * 동작 단계:
 *   1) nvme_ctrlr_get_by_name으로 컨트롤러 lookup, opal_dev 존재 확인.
 *   2) spdk_opal_cmd_setup_locking_range로 Locking Range 메타데이터 설정.
 *   3) range_start/length가 namespace 크기를 넘지 않는지 검증.
 *   4) 새 vbdev 구조체 할당 및 fn_table 채워서 spdk_bdev_register 호출.
 * 실행 컨텍스트: app 스레드 (RPC 핸들러에서 직접 호출).
 */
int vbdev_opal_create(const char *nvme_ctrlr_name, uint32_t nsid, uint8_t locking_range_id,
		      uint64_t range_start, uint64_t range_length, const char *password);

/*
 * [한국어]
 * vbdev_opal_get_info_from_bdev - Opal Locking Range의 현재 잠금/암호화 상태 조회.
 *
 * @opal_bdev_name: 조회 대상 vbdev 이름 (vbdev_opal_create에서 만든 이름).
 * @password:       Admin/User 인증 패스워드.
 * @return: spdk_opal_locking_range_info 포인터 (성공 시 정적/할당된 정보 구조체),
 *          NULL이면 인증 실패 또는 bdev not found.
 *
 * 호출자는 반환된 info 구조체에서 read_locked/write_locked/range_start 등을 RPC
 * 응답 JSON으로 직렬화한다. 이 함수는 SED에 Get-Range 명령을 발급하므로 약간의
 * 지연이 발생할 수 있다.
 * 실행 컨텍스트: app 스레드 (RPC 핸들러).
 */
struct spdk_opal_locking_range_info *vbdev_opal_get_info_from_bdev(const char *opal_bdev_name,
		const char *password);

/*
 * [한국어]
 * vbdev_opal_destruct - vbdev_opal bdev를 unregister하고 Locking Range를 정리.
 *
 * @bdev_name: 제거할 vbdev 이름.
 * @password:  Admin SP 인증 패스워드 (잠금 복원에 필요).
 * @return: 0 성공, 음수 errno.
 *
 * 동작 단계:
 *   1) bdev lookup 후 Opal 인증.
 *   2) spdk_bdev_unregister 호출 (열린 descriptor가 있으면 콜백을 통해 지연 해제).
 *   3) Locking Range를 다시 잠그고(SED 펌웨어 측), 모듈 컨텍스트 메모리 해제.
 * 실행 컨텍스트: app 스레드.
 */
int vbdev_opal_destruct(const char *bdev_name, const char *password);

/*
 * [한국어]
 * vbdev_opal_enable_new_user - Locking Range에 추가 User 슬롯의 사용 권한을 부여한다.
 *
 * @bdev_name:       대상 vbdev 이름.
 * @admin_password:  Admin SP 인증 패스워드 (권한 변경 시 필요).
 * @user_id:         활성화할 User 슬롯 번호 (예: User1=1, User2=2 …).
 * @user_password:   해당 User에 부여할 새 패스워드.
 * @return: 0 성공, 음수 errno (인증 실패/잘못된 user_id 등).
 *
 * SED 내부적으로 User Authority 활성화, ACL 갱신, 패스워드 설정 3단계가 발생한다.
 * 같은 Locking Range에 여러 User가 read/write 권한을 가질 수 있도록 한다.
 * 실행 컨텍스트: app 스레드.
 */
int vbdev_opal_enable_new_user(const char *bdev_name, const char *admin_password,
			       uint16_t user_id, const char *user_password);

/*
 * [한국어]
 * vbdev_opal_set_lock_state - Locking Range의 read/write 잠금 상태를 변경한다.
 *
 * @bdev_name:  대상 vbdev 이름.
 * @user_id:    인증을 수행할 User ID (Admin은 0 또는 별도 표기, 구현 참조).
 * @password:   해당 user의 패스워드.
 * @lock_state: 문자열 - "RWLOCK"(read+write 잠금), "READONLY", "RWUNLOCK"(완전 해제) 등.
 * @return: 0 성공, 음수 errno.
 *
 * 잠긴 상태에서 호스트가 해당 LBA에 접근하면 SED가 명령을 거부한다. 잠금 해제 시
 * 평문 read/write가 가능해지지만 디스크상 데이터는 여전히 SED 키로 암호화되어 있다.
 * 실행 컨텍스트: app 스레드.
 */
int vbdev_opal_set_lock_state(const char *bdev_name, uint16_t user_id, const char *password,
			      const char *lock_state);

#endif /* [한국어] SPDK_VBDEV_OPAL_H 다중 포함 방지 가드 종료. */
