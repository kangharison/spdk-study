/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL의 평탄(flat) L2P(Logical-to-Physical) 매핑 테이블 구현 (ftl_l2p_flat.c)
 *
 * === 파일의 역할 ===
 * SPDK FTL(Flash Translation Layer)에서 LBA → 물리 주소(ftl_addr) 매핑을 단일
 * 평탄 배열로 구현하는 백엔드이다. L2P 테이블 전체를 DRAM(혹은 SHM 매핑 영역)에
 * 한꺼번에 펼쳐 두는 가장 단순한 구현으로, LBA 인덱스로 즉시 ftl_addr_size 바이트
 * 슬롯을 읽거나 쓴다. 별도의 캐시 계층이 없으므로 pin/unpin은 no-op이며, 디바이스
 * 용량에 비례한 DRAM이 항상 점유되는 트레이드오프가 있다. 빌드 옵션 SPDK_FTL_L2P_FLAT
 * 가 정의된 경우 ftl_l2p.c의 디스패처가 이 백엔드를 선택한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * FTL 사용자 I/O 경로:
 *   bdev_ftl(read/write) → ftl_io_init → ftl_l2p_pin/get/set → ftl_l2p_flat_*
 * 즉 본 파일은 ftl_l2p.c의 함수 디스패처(FTL_L2P_OP) 뒤에 위치하는 "L2P provider"이다.
 * 메타데이터 영역 측면에서는 layout.md[FTL_LAYOUT_REGION_TYPE_L2P]가 가리키는
 * ftl_md(L2P 메타데이터 객체)의 버퍼를 직접 매핑하여 사용한다. persist/restore
 * 시에는 ftl_md_persist/ftl_md_restore를 호출하여 베이스 디바이스의 메타데이터
 * 영역에 L2P 전체를 그대로 쓰거나 읽어온다.
 * 실행 컨텍스트: SPDK FTL의 코어 스레드(dev->core_thread) 위에서만 호출된다.
 * polled-mode 단일 스레드 모델이므로 별도의 락 없이 안전하게 접근 가능하다.
 *
 * === 타 모듈과의 연결 ===
 * - ftl_l2p.c: 디스패처. SPDK_FTL_L2P_FLAT 매크로가 정의되면 본 파일의 함수를 호출.
 * - ftl_md (lib/ftl/ftl_md.c): persist/restore/get_buffer를 통해 L2P 영역의
 *   영속화 및 메모리 매핑을 담당.
 * - utils/ftl_addr_utils.h: ftl_addr_load/store가 ftl_addr_size(4 또는 8바이트)에
 *   따라 슬롯에 접근하는 추상화를 제공.
 * - ftl_band/ftl_nv_cache: L2P 값(ftl_addr)이 가리키는 물리 위치 관리 모듈.
 * 데이터 흐름: 사용자 LBA → l2p_flat->l2p[lba] = ftl_addr (인덱싱 직접) →
 * 베이스 SSD/캐시 디바이스의 물리 주소.
 * 공유 자료구조: dev->l2p (void*) — 본 파일이 ftl_l2p_flat 인스턴스로 채움.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ftl_l2p_flat: l2p 버퍼 포인터와 halt 상태 플래그.
 * - ftl_l2p_flat_init/deinit: ftl_md 버퍼를 빌려와 dev->l2p에 설치/해제.
 * - ftl_l2p_flat_get/set: O(1) 인덱싱으로 LBA→ftl_addr 변환/갱신.
 * - ftl_l2p_flat_pin/unpin: flat 백엔드에서는 캐시가 없으므로 즉시 완료(no-op).
 * - ftl_l2p_flat_clear/restore/persist: ftl_md를 통해 L2P 영역 일괄 영속화.
 * - ftl_l2p_flat_trim/process/halt/resume/is_halted: 캐시 백엔드와의 인터페이스
 *   호환을 위한 stub. flat에서는 비동기 작업이 없어 대부분 no-op.
 */

#include "ftl_l2p.h"            /* [한국어] L2P 디스패처 인터페이스(ftl_l2p_pin_ctx, ftl_l2p_cb 등). */
#include "ftl_core.h"           /* [한국어] struct spdk_ftl_dev 정의 — dev->l2p, dev->layout 접근에 필요. */
#include "ftl_band.h"           /* [한국어] 밴드(band) 관련 — ftl_addr 해석 시 사용. */
#include "ftl_utils.h"          /* [한국어] FTL_ERRLOG/FTL_NOTICELOG 등 로깅 매크로. */
#include "ftl_l2p_flat.h"       /* [한국어] 본 파일이 외부에 노출하는 ftl_l2p_flat_* 프로토타입. */
#include "utils/ftl_addr_utils.h" /* [한국어] ftl_addr_load/store — addr_size(4/8B)에 따라 슬롯 read/write 추상화. */

/*
 * [한국어]
 * get_l2p_md - dev->layout에서 L2P 영역의 ftl_md 객체를 반환
 *
 * @dev: SPDK FTL 디바이스 핸들. layout.md 배열은 디바이스 초기화 시 채워짐.
 * @return: L2P 메타데이터 영역(FTL_LAYOUT_REGION_TYPE_L2P)에 해당하는 ftl_md 포인터.
 *
 * 이 헬퍼는 본 파일 전체에서 반복적으로 사용되는 layout.md[L2P] 접근을 한 곳에 모은다.
 * ftl_md는 베이스 디바이스의 특정 영역에 대한 영속 매핑/버퍼/persist 콜백을 묶은 객체로,
 * 여기서 얻어진 md를 통해 L2P 전체에 대한 in-memory 버퍼와 영속화 작업을 수행한다.
 * 실행 컨텍스트: 코어 스레드 한정(단일 스레드 → 락 불필요).
 *
 * 호출 체인:
 *   ftl_l2p_flat_clear/restore/persist/init_dram → [get_l2p_md] → ftl_md_*
 */
static struct ftl_md *
get_l2p_md(struct spdk_ftl_dev *dev)
{
	return dev->layout.md[FTL_LAYOUT_REGION_TYPE_L2P]; /* [한국어] L2P 메타 영역 인덱스로 ftl_md 포인터 반환. */
}

/*
 * [한국어]
 * struct ftl_l2p_flat - flat L2P 백엔드의 인스턴스 상태
 *
 * dev->l2p에 저장되어 본 파일의 모든 호출에서 첫 단계로 다운캐스팅되는 객체.
 * 캐시 백엔드(ftl_l2p_cache)와 같은 슬롯에 들어가지만 멤버는 훨씬 단순하다.
 */
struct ftl_l2p_flat {
	void *l2p;
	/* [한국어] L2P 평탄 배열의 시작 주소.
	 * 설정자: ftl_l2p_flat_init_dram()이 ftl_md_get_buffer(get_l2p_md(dev))로 설정.
	 * 읽는 자: ftl_addr_load/store가 (lba * addr_size) 오프셋으로 접근.
	 * 값 범위: ftl_md가 관리하는 in-memory 버퍼(베이스 SSD 영역 또는 SHM 매핑).
	 *   크기는 dev->num_lbas * dev->layout.l2p.addr_size 바이트 이상이 보장됨.
	 * 동기화: 코어 스레드 단일 접근 — 락 없음. */

	bool is_halted;
	/* [한국어] 백엔드가 halt 상태인지 표시 (현재 코드에서는 갱신되지 않음).
	 * 설정자: 미사용 (확장 여지로 남겨진 필드).
	 * 읽는 자: ftl_l2p_flat_is_halted() 후보였으나 현재는 항상 true 반환.
	 * 값 범위: true/false. flat 백엔드는 비동기 작업이 없어 항상 즉시 halt 가능. */
};

/*
 * [한국어]
 * ftl_l2p_flat_pin - 사용자 I/O 처리 전 LBA 범위를 "고정"하는 진입점 (flat은 no-op)
 *
 * @dev: FTL 디바이스 핸들.
 * @pin_ctx: 핀 요청 컨텍스트. lba/count/cb/cb_ctx가 ftl_l2p_pin()에서 미리 채워짐.
 * @return: 없음. 즉시 pin_ctx->cb(dev, 0, pin_ctx)가 호출되어 동기적으로 완료됨.
 *
 * cache 백엔드에서는 LBA가 속한 L2P 페이지가 메모리에 없을 수 있어 pin이 비동기로
 * 페이지를 로드한다. flat 백엔드는 L2P 전체가 항상 메모리에 상주하므로 핀할 것이
 * 아무것도 없다. 따라서 pin_complete(상태 0)만 호출해 호출자에 즉시 진행을 알린다.
 * 실행 컨텍스트: 코어 스레드. ftl_l2p_pin()을 통해 사용자/GC/컴팩션 경로 모두에서 호출.
 *
 * 호출 체인:
 *   ftl_io_pin / GC / compaction → ftl_l2p_pin → [ftl_l2p_flat_pin]
 *     → ftl_l2p_pin_complete → pin_ctx->cb (동기 완료)
 */
void
ftl_l2p_flat_pin(struct spdk_ftl_dev *dev, struct ftl_l2p_pin_ctx *pin_ctx)
{
	assert(dev->num_lbas >= pin_ctx->lba + pin_ctx->count); /* [한국어] 요청 범위가 디바이스 LBA 범위 내인지 확인 — 오버플로/오류 방지. */

	ftl_l2p_pin_complete(dev, 0, pin_ctx); /* [한국어] 즉시 완료 통지. status=0(성공). cache 백엔드와 동일한 콜백 시그니처 유지. */
}

/*
 * [한국어]
 * ftl_l2p_flat_unpin - pin과 짝이 되는 unpin 진입점 (flat은 no-op)
 *
 * @dev: FTL 디바이스 핸들.
 * @lba: 핀 해제할 시작 LBA.
 * @count: 핀 해제할 LBA 개수.
 * @return: 없음.
 *
 * cache 백엔드는 LBA 페이지의 참조 카운트를 감소시키고 0이 되면 evict 후보로 만든다.
 * flat 백엔드는 페이지 캐싱이 없으므로 검증만 수행하고 실제 작업은 없다.
 * 실행 컨텍스트: 코어 스레드.
 *
 * 호출 체인:
 *   ftl_io_complete → ftl_l2p_unpin → [ftl_l2p_flat_unpin]
 */
void
ftl_l2p_flat_unpin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count)
{
	assert(dev->num_lbas >= lba + count); /* [한국어] 범위 검증만 수행. flat에는 unpin할 상태가 없음. */
}

/*
 * [한국어]
 * ftl_l2p_flat_set - 특정 LBA의 L2P 매핑을 새 물리 주소로 갱신
 *
 * @dev: FTL 디바이스 핸들. dev->l2p가 본 백엔드 인스턴스로 설치되어 있어야 함.
 * @lba: 갱신 대상 논리 블록 주소.
 * @addr: 새 물리 주소(ftl_addr) — 베이스 밴드 또는 NV cache 위치.
 * @return: 없음.
 *
 * 사용자 쓰기/GC/컴팩션이 데이터를 새 위치에 기록한 뒤 L2P를 갱신할 때 호출된다.
 * ftl_addr_store는 dev->layout.l2p.addr_size(4 또는 8바이트)에 따라
 * l2p_flat->l2p의 (lba * addr_size) 오프셋에 addr를 안전하게 기록한다.
 * 실행 컨텍스트: 코어 스레드. ftl_l2p.c의 update_cache/update_base 등에서 호출.
 *
 * 호출 체인:
 *   ftl_l2p_update_cache/update_base → ftl_l2p_set → [ftl_l2p_flat_set]
 *     → ftl_addr_store
 */
void
ftl_l2p_flat_set(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p; /* [한국어] dev->l2p (void*)를 본 백엔드 타입으로 다운캐스팅. */

	assert(dev->num_lbas > lba); /* [한국어] LBA 범위 검사 — 디바이스 LBA 한도를 넘지 않음을 확인. */

	ftl_addr_store(dev, l2p_flat->l2p, lba, addr); /* [한국어] addr_size에 맞춰 l2p[lba] 슬롯에 addr 기록. addr_size는 디바이스 용량에 따라 결정. */
}

/*
 * [한국어]
 * ftl_l2p_flat_get - LBA의 현재 L2P 매핑(물리 주소)을 조회
 *
 * @dev: FTL 디바이스 핸들.
 * @lba: 조회할 논리 블록 주소.
 * @return: 매핑된 ftl_addr. 매핑 없음/지워짐 상태면 FTL_ADDR_INVALID.
 *
 * 사용자 읽기, write-after-write 검증, L2P 무효화 검사 등 모든 L2P 조회의 종착점이다.
 * ftl_addr_load는 addr_size에 따라 4 또는 8바이트를 읽어 ftl_addr(uint64_t)로 확장한다.
 * 실행 컨텍스트: 코어 스레드. 단일 스레드 접근이므로 라인 단위 일관성이 자동 보장.
 *
 * 호출 체인:
 *   사용자 read 핸들러/검증 → ftl_l2p_get → [ftl_l2p_flat_get] → ftl_addr_load
 */
ftl_addr
ftl_l2p_flat_get(struct spdk_ftl_dev *dev, uint64_t lba)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p; /* [한국어] 백엔드 인스턴스 포인터 획득. */

	assert(dev->num_lbas > lba); /* [한국어] LBA 범위 검증. */

	return ftl_addr_load(dev, l2p_flat->l2p, lba); /* [한국어] l2p[lba] 슬롯에서 addr_size 바이트를 읽어 ftl_addr로 반환. */
}

/*
 * [한국어]
 * md_cb - ftl_md persist/restore 비동기 완료 시 호출되는 트램폴린
 *
 * @dev: FTL 디바이스 핸들 (ftl_md가 콜백 인자로 전달).
 * @md: 작업이 완료된 ftl_md 객체. owner.private/owner.cb_ctx에 사용자 콜백 정보가 저장.
 * @status: 0이면 성공, 음수면 errno (예: -EIO).
 *
 * ftl_md는 일반화된 메타데이터 영역 매니저로, 작업 완료 시 md->cb를 호출한다.
 * 본 파일은 사용자가 ftl_l2p_clear/restore/persist에 넘긴 ftl_l2p_cb를
 * md->owner.private에, cb_ctx를 md->owner.cb_ctx에 임시로 저장한 뒤,
 * 이 트램폴린에서 사용자 콜백 시그니처(cb(dev, status, cb_ctx))로 변환해 호출한다.
 * 실행 컨텍스트: ftl_md의 비동기 처리가 완료되는 스레드(통상 코어 스레드).
 *
 * 호출 체인:
 *   ftl_md_persist/restore → (비동기) → md->cb == [md_cb] → 사용자 ftl_l2p_cb
 */
static void
md_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	ftl_l2p_cb cb = md->owner.private;       /* [한국어] persist/restore 호출 시 owner.private에 저장한 사용자 콜백 복원. */
	void *cb_ctx = md->owner.cb_ctx;         /* [한국어] 동일하게 사용자 cb_ctx 복원. */

	cb(dev, status, cb_ctx);                 /* [한국어] 사용자 콜백 호출. status는 ftl_md가 전달한 작업 결과 그대로 전파. */
}

/*
 * [한국어]
 * ftl_l2p_flat_clear - L2P 전체를 INVALID로 초기화하고 영속화
 *
 * @dev: FTL 디바이스 핸들.
 * @cb: 영속화 완료 시 호출될 사용자 콜백 (ftl_l2p_cb 시그니처).
 * @cb_ctx: 사용자 콜백에 전달될 컨텍스트 포인터.
 * @return: 없음 (비동기 — 완료는 cb로 통지).
 *
 * 디바이스 첫 포맷/리셋 흐름에서 호출된다. memset으로 in-memory L2P를 0xFF…(=FTL_ADDR_INVALID)로
 * 채운 뒤 ftl_md_persist를 호출해 베이스 디바이스의 L2P 영역을 동일하게 기록한다.
 * 영속화가 끝나면 md_cb를 거쳐 사용자 cb가 호출된다.
 * 실행 컨텍스트: 코어 스레드. 관리(mngt) FSM의 한 단계로 호출됨.
 *
 * 호출 체인:
 *   ftl_mngt_* (포맷/초기화 단계) → ftl_l2p_clear → [ftl_l2p_flat_clear]
 *     → ftl_md_persist → (비동기) md_cb → cb
 */
void
ftl_l2p_flat_clear(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p; /* [한국어] 백엔드 인스턴스 포인터. */
	struct ftl_md *md;                        /* [한국어] L2P 메타 영역 ftl_md 핸들 (아래에서 채움). */

	memset(l2p_flat->l2p, (int)FTL_ADDR_INVALID,
	       ftl_md_get_buffer_size(get_l2p_md(dev)));
	/* [한국어] L2P 전체를 0xFF…(FTL_ADDR_INVALID)로 채움.
	 * memset의 두 번째 인자는 바이트 단위로 반복되므로, FTL_ADDR_INVALID가
	 * 0xFFFFFFFF(또는 0xFFFFFFFFFFFFFFFF) 형태인 것을 전제로 안전하게 동작한다.
	 * 버퍼 크기는 ftl_md가 관리하는 정확한 바이트 수. */

	md = get_l2p_md(dev);                     /* [한국어] persist 호출을 위한 md 객체 획득. */
	md->cb = md_cb;                           /* [한국어] 완료 트램폴린 등록. */
	md->owner.cb_ctx = cb_ctx;                /* [한국어] 사용자 cb_ctx 임시 보관 (md_cb에서 꺼내씀). */
	md->owner.private = cb;                   /* [한국어] 사용자 콜백 자체도 owner.private에 보관. */
	ftl_md_persist(md);                       /* [한국어] 베이스 디바이스 L2P 영역에 in-memory 버퍼를 비동기로 기록. */
}

/*
 * [한국어]
 * ftl_l2p_flat_restore - 베이스 디바이스에 저장된 L2P를 메모리로 복원
 *
 * @dev: FTL 디바이스 핸들.
 * @cb: 복원 완료 시 호출될 사용자 콜백.
 * @cb_ctx: 사용자 콜백 컨텍스트.
 * @return: 없음 (비동기).
 *
 * 정상 종료 후 재시작(clean shutdown recovery) 또는 dirty shutdown 복구의 한 단계.
 * ftl_md_restore가 베이스 SSD의 L2P 영역을 in-memory 버퍼(=l2p_flat->l2p)로 읽어들이고,
 * 완료 시 md_cb를 통해 사용자 cb가 호출된다.
 * 실행 컨텍스트: 코어 스레드. 관리 FSM의 startup 단계에서 호출.
 *
 * 호출 체인:
 *   ftl_mngt_* (startup) → ftl_l2p_restore → [ftl_l2p_flat_restore]
 *     → ftl_md_restore → (비동기) md_cb → cb
 */
void
ftl_l2p_flat_restore(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_md *md; /* [한국어] L2P 영역 ftl_md 핸들. */

	md = get_l2p_md(dev);            /* [한국어] L2P 메타 객체 획득. */
	md->cb = md_cb;                  /* [한국어] 비동기 완료 시 md_cb로 진입하도록 설정. */
	md->owner.cb_ctx = cb_ctx;       /* [한국어] 사용자 cb_ctx 임시 저장. */
	md->owner.private = cb;          /* [한국어] 사용자 ftl_l2p_cb 임시 저장. */
	ftl_md_restore(md);              /* [한국어] 베이스 SSD에서 L2P 영역을 읽어 버퍼에 복원. */
}

/*
 * [한국어]
 * ftl_l2p_flat_persist - 현재 in-memory L2P를 베이스 디바이스에 영속화
 *
 * @dev: FTL 디바이스 핸들.
 * @cb: 완료 콜백.
 * @cb_ctx: 콜백 컨텍스트.
 * @return: 없음 (비동기).
 *
 * shutdown 시 또는 주기적 체크포인트에서 호출되어, 메모리에 들고 있는 L2P 전체를
 * 베이스 SSD에 그대로 기록한다. flat은 변경 페이지 추적이 없으므로 영역 전체가 대상이다.
 * 실행 컨텍스트: 코어 스레드. 관리 FSM의 shutdown 또는 sync 단계에서 호출.
 *
 * 호출 체인:
 *   ftl_mngt_* (shutdown/sync) → ftl_l2p_persist → [ftl_l2p_flat_persist]
 *     → ftl_md_persist → (비동기) md_cb → cb
 */
void
ftl_l2p_flat_persist(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_md *md; /* [한국어] L2P ftl_md 핸들. */

	md = get_l2p_md(dev);            /* [한국어] L2P 메타 객체 획득. */
	md->cb = md_cb;                  /* [한국어] 완료 트램폴린 등록. */
	md->owner.cb_ctx = cb_ctx;       /* [한국어] 사용자 cb_ctx 임시 저장. */
	md->owner.private = cb;          /* [한국어] 사용자 ftl_l2p_cb 임시 저장. */
	ftl_md_persist(md);              /* [한국어] in-memory 버퍼를 베이스 SSD L2P 영역에 비동기 기록. */
}

/*
 * [한국어]
 * ftl_l2p_flat_init_dram - 백엔드 인스턴스의 l2p 포인터를 ftl_md 버퍼로 채움
 *
 * @dev: FTL 디바이스 핸들.
 * @l2p_flat: 본 파일이 calloc으로 갓 할당한 인스턴스 (l2p가 아직 NULL).
 * @l2p_size: 필요로 하는 L2P 바이트 수 (= num_lbas * addr_size).
 * @return: 0 성공, -1 실패.
 *
 * ftl_md는 디바이스 layout 단계에서 이미 충분한 크기의 in-memory 버퍼를 확보해 두므로,
 * 여기서는 그 버퍼를 빌려와 l2p 포인터에 그대로 꽂는다(zero-copy 성격).
 * 따라서 ftl_l2p_flat 자체가 별도 버퍼를 소유하지 않고, 수명은 ftl_md가 관리한다.
 * 실행 컨텍스트: 코어 스레드. ftl_l2p_flat_init()에서만 호출되는 정적 헬퍼.
 *
 * 호출 체인:
 *   ftl_l2p_flat_init → [ftl_l2p_flat_init_dram] → ftl_md_get_buffer
 */
static int
ftl_l2p_flat_init_dram(struct spdk_ftl_dev *dev, struct ftl_l2p_flat *l2p_flat,
		       size_t l2p_size)
{
	struct ftl_md *md = get_l2p_md(dev); /* [한국어] L2P 영역 ftl_md 핸들. */

	assert(ftl_md_get_buffer_size(md) >= l2p_size); /* [한국어] layout 단계에서 충분한 크기로 잡혔는지 검증. */

	l2p_flat->l2p = ftl_md_get_buffer(md); /* [한국어] ftl_md가 보유한 in-memory 버퍼 포인터를 그대로 가져옴 (zero-copy). */
	if (!l2p_flat->l2p) {                  /* [한국어] 버퍼가 아직 매핑되지 않은 비정상 상황. */
		FTL_ERRLOG(dev, "Failed to allocate l2p table\n"); /* [한국어] 에러 로그. */
		return -1;                                          /* [한국어] 호출자에 실패 통지. */
	}

	return 0; /* [한국어] 성공. */
}

/*
 * [한국어]
 * ftl_l2p_flat_init - flat L2P 백엔드 초기화 진입점
 *
 * @dev: FTL 디바이스 핸들. dev->num_lbas와 layout이 준비된 상태여야 함.
 * @return: 0 성공, -1 실패 (잘못된 size, 중복 할당, malloc 실패, 버퍼 매핑 실패).
 *
 * ftl_l2p_init() 내부의 FTL_L2P_OP(init) 매크로 분기에서 호출된다.
 * 1) 디바이스 크기/중복 할당 검증
 * 2) ftl_l2p_flat 인스턴스 calloc
 * 3) ftl_md 버퍼와 연결 (init_dram)
 * 4) dev->l2p에 인스턴스 설치
 * 실행 컨텍스트: 코어 스레드. 관리 FSM의 startup 초반에 호출.
 *
 * 호출 체인:
 *   ftl_mngt_* (startup) → ftl_l2p_init → [ftl_l2p_flat_init]
 *     → ftl_l2p_flat_init_dram
 */
int
ftl_l2p_flat_init(struct spdk_ftl_dev *dev)
{
	size_t l2p_size = dev->num_lbas * dev->layout.l2p.addr_size; /* [한국어] L2P 전체 크기 = LBA 개수 × 슬롯당 바이트. */
	struct ftl_l2p_flat *l2p_flat;                               /* [한국어] 새로 할당할 백엔드 인스턴스. */
	int ret;                                                     /* [한국어] init_dram 반환값 보관. */

	if (dev->num_lbas == 0) {                       /* [한국어] LBA가 0인 디바이스는 비정상 — 초기화 거부. */
		FTL_ERRLOG(dev, "Invalid l2p table size\n");
		return -1;
	}

	if (dev->l2p) {                                 /* [한국어] 이미 백엔드가 설치되어 있으면 중복 초기화 — 오류. */
		FTL_ERRLOG(dev, "L2p table already allocated\n");
		return -1;
	}

	l2p_flat = calloc(1, sizeof(*l2p_flat));        /* [한국어] 인스턴스 할당. calloc으로 is_halted 등 0 초기화. */
	if (!l2p_flat) {                                /* [한국어] 메모리 부족 처리. */
		FTL_ERRLOG(dev, "Failed to allocate l2p_flat\n");
		return -1;
	}

	ret = ftl_l2p_flat_init_dram(dev, l2p_flat, l2p_size); /* [한국어] ftl_md 버퍼와 연결. */

	if (ret) {                                      /* [한국어] 실패 시 인스턴스를 회수하고 종료. */
		free(l2p_flat);
		return ret;
	}

	dev->l2p = l2p_flat;                            /* [한국어] 디스패처(ftl_l2p.c)가 dev->l2p를 통해 본 백엔드를 사용하도록 설치. */
	return 0;                                        /* [한국어] 초기화 성공. */
}

/*
 * [한국어]
 * ftl_l2p_flat_deinit - flat L2P 백엔드 해제
 *
 * @dev: FTL 디바이스 핸들.
 * @return: 없음.
 *
 * shutdown 또는 init 실패 롤백 시 호출. 인스턴스 자체만 해제하며, l2p_flat->l2p가
 * 가리키던 ftl_md 버퍼는 ftl_md 측에서 별도로 해제하므로 여기서는 건드리지 않는다.
 * 실행 컨텍스트: 코어 스레드.
 *
 * 호출 체인:
 *   ftl_mngt_* (shutdown) → ftl_l2p_deinit → [ftl_l2p_flat_deinit]
 */
void
ftl_l2p_flat_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_flat *l2p_flat = dev->l2p; /* [한국어] 현재 설치된 인스턴스. */

	if (!l2p_flat) {                          /* [한국어] 미설치 상태(이미 해제 또는 init 실패)면 idempotent하게 종료. */
		return;
	}

	free(l2p_flat);                           /* [한국어] 인스턴스 해제. l2p 버퍼 자체는 ftl_md가 관리하므로 free하지 않음. */

	dev->l2p = NULL;                          /* [한국어] 더 이상 백엔드가 없음을 알림 — 재초기화 검증에도 사용됨. */
}

/*
 * [한국어]
 * ftl_l2p_flat_trim - TRIM(=논리 삭제) 처리 (flat은 즉시 완료)
 *
 * @dev: FTL 디바이스 핸들.
 * @cb: 완료 콜백.
 * @cb_ctx: 콜백 컨텍스트.
 * @return: 없음 (콜백 즉시 호출).
 *
 * cache 백엔드는 trim 시 페이지 캐시 동기화/seq_id 갱신 등 비동기 작업이 필요하다.
 * flat 백엔드는 별도 trim 메타가 필요 없어 ftl_l2p.c 상위 레이어가 처리하고,
 * 이 함수는 단순히 즉시 완료를 통지한다.
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_l2p_flat_trim(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	cb(dev, 0, cb_ctx); /* [한국어] flat에서는 trim 후속 처리가 없어 즉시 성공 통지. */
}

/*
 * [한국어]
 * ftl_l2p_flat_process - poller 주기 처리 훅 (flat은 no-op)
 *
 * @dev: FTL 디바이스 핸들.
 * @return: 없음.
 *
 * cache 백엔드는 매 poller tick마다 진행 중인 페이지 fetch/evict 상태를 한 단계씩 진행한다.
 * flat은 비동기 진행 상태가 없으므로 빈 함수.
 * 실행 컨텍스트: 코어 스레드의 메인 poller에서 매 tick 호출.
 */
void
ftl_l2p_flat_process(struct spdk_ftl_dev *dev)
{
}

/*
 * [한국어]
 * ftl_l2p_flat_is_halted - 백엔드가 진행 중 작업 없이 정지 상태인지
 *
 * @dev: FTL 디바이스 핸들.
 * @return: 항상 true. flat은 비동기 작업 자체가 없어 즉시 halt가 보장됨.
 *
 * shutdown 시 상위 레이어가 폴링하여 안전하게 종료할 수 있는 시점을 판단하는 데 사용.
 * 실행 컨텍스트: 코어 스레드.
 */
bool
ftl_l2p_flat_is_halted(struct spdk_ftl_dev *dev)
{
	return true; /* [한국어] flat은 진행 중인 비동기 작업이 없어 항상 halt 상태. */
}

/*
 * [한국어]
 * ftl_l2p_flat_halt - 백엔드 정지 요청 (flat은 즉시 정지 상태이므로 no-op)
 *
 * @dev: FTL 디바이스 핸들.
 * @return: 없음.
 *
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_l2p_flat_halt(struct spdk_ftl_dev *dev)
{
}

/*
 * [한국어]
 * ftl_l2p_flat_resume - halt 해제/재개 (flat은 항상 동작 가능하므로 no-op)
 *
 * @dev: FTL 디바이스 핸들.
 * @return: 없음.
 *
 * 실행 컨텍스트: 코어 스레드.
 */
void
ftl_l2p_flat_resume(struct spdk_ftl_dev *dev)
{
}
