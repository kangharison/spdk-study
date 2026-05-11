/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] split vbdev 모듈 공개 헤더 (vbdev_split.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 split(파티셔닝) 가상 bdev 모듈이 외부(JSON-RPC 핸들러)에 노출하는
 * 세 함수의 선언만 담는다. split 모듈은 한 개의 base bdev를 N개의 동일/지정 크기
 * 서브-bdev로 잘라 각 파티션을 독립된 spdk_bdev 처럼 보이게 한다.
 * 내부적으로는 SPDK 공통 파티션 라이브러리인 lib/bdev/part.c (spdk_bdev_part_base /
 * spdk_bdev_part 자료구조)에 위임하며, split.c는 그 위에 split 특유의 정책(균등 분할,
 * 갯수, MB 단위 크기 지정)만 얹는 박싱 레이어다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK bdev 스택의 "vbdev 모듈" 자리. 한 base bdev를 여러 part로 쪼갠다는 점이
 * passthru/delay/error와의 차이점. 호출 체인은:
 * 사용자 → JSON-RPC ("bdev_split_create"/"bdev_split_delete") →
 * vbdev_split_rpc.c → 본 헤더의 create_vbdev_split()/vbdev_split_destruct() →
 * vbdev_split.c → spdk_bdev_part_base_construct_ext() & spdk_bdev_part_construct()
 * (lib/bdev/part.c) → spdk_bdev_register(). I/O 경로에서 제출된 bdev_io는
 * part 레이어에서 base 오프셋을 더해 base bdev로 재발행된다.
 * 실행 컨텍스트: 본 헤더의 함수는 모두 SPDK app 스레드(주로 RPC/init 스레드)에서
 * 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: <spdk/bdev_module.h>(spdk_bdev_part_base, spdk_bdev_part, bdev 등록 매크로).
 * 본 헤더에 의존하는 측: vbdev_split.c, vbdev_split_rpc.c. RPC 핸들러는 이 함수를 통해
 * config(base 이름, 분할 수, 분할 MB)을 모듈에 주입한다. 또한
 * vbdev_split_get_part_base()는 RPC 응답에 split 결과(파티션 이름 목록)를 채우기
 * 위한 인트로스펙션 함수로 사용된다.
 * 데이터 흐름: 사용자 입력 → split 모듈 내부 splits 리스트 → spdk_bdev_part_base 등록
 * → 각 파티션이 spdk_bdev로 나타나 다른 모듈/애플리케이션이 사용 가능.
 * 공유 구조체: spdk_bdev_part_base / spdk_bdev_part / bdev_part_tailq (lib/bdev/part.c
 * 참고 — 이미 주석 작업된 핵심 자료구조).
 *
 * === 주요 함수/구조체 요약 ===
 * - create_vbdev_split(): base bdev에 대한 split 설정을 추가. base가 이미 등록돼 있으면
 *   즉시 분할 vbdev를 만들고, 아직 없으면 examine 단계에서 만들도록 예약.
 * - vbdev_split_destruct(): base에 묶여 있는 모든 split vbdev와 split 설정을 제거.
 * - vbdev_split_get_part_base(): 주어진 base bdev에 연결된 spdk_bdev_part_base를 조회.
 *   주로 RPC가 생성된 파티션 이름 리스트를 응답으로 채울 때 사용.
 * 본 헤더에는 구조체 정의는 없다 — split 내부 구조체는 .c 파일에 비공개.
 */

#ifndef SPDK_VBDEV_SPLIT_H
/* [한국어] 헤더 가드 시작 — 중복 포함 방지. */
#define SPDK_VBDEV_SPLIT_H

#include "spdk/bdev_module.h"
/* [한국어] bdev 모듈 작성용 내부 API. spdk_bdev_part_base/spdk_bdev_part 같은
 * 본 헤더 함수의 반환/파라미터 타입을 정의하므로 필수. */

/**
 * Add given disk name to split config. If bdev with \c base_bdev_name name
 * exist the split bdevs will be created right away, if not the split bdevs will
 * be created when base bdev became be available (during examination process).
 *
 * \param base_bdev_name Base bdev name
 * \param split_count number of splits to be created.
 * \param split_size_mb size of each bdev. If 0 use base bdev size / split_count
 * \return value >= 0 - number of splits create. Negative errno code on error.
 */
/*
 * [한국어]
 * create_vbdev_split - base bdev에 split 설정을 등록하고 가능하면 즉시 분할 생성.
 *
 * @base_bdev_name: 분할 대상 base bdev 이름. 호출 시점에 등록돼 있지 않아도
 *                  예약(deferred)되며, 이후 base bdev가 등록되면 examine 콜백이
 *                  자동으로 split을 만든다.
 * @split_count: 생성할 파티션 개수(>=1). 보통 1~수십 단위.
 * @split_size_mb: 파티션 1개의 크기 — MB 단위. 0이면 base 크기 / split_count로 균등.
 *                 0이 아닐 때 split_count * split_size_mb가 base 크기를 넘으면 실패.
 * @return: 0 이상 — 즉시 만들어진 파티션 수(즉시 만들지 못해도 0 가능). 음수 — 실패 errno.
 *
 * 동기/배경: SPDK 모듈은 base bdev가 하나만 있을 때 그것을 여러 부분으로 노출해야
 * 하는 경우가 많다(예: 라벨/메타/데이터 영역). 이 함수는 그런 정책을 한 번에 등록한다.
 * 실행 컨텍스트: SPDK app 스레드. 내부에서 spdk_bdev_part_base_construct_ext()로
 * base에 대한 part_base를 만들고, 각 파티션마다 spdk_bdev_part_construct()를 호출해
 * spdk_bdev로 등록한다.
 *
 * 호출 체인:
 *   rpc_bdev_split_create() → [create_vbdev_split] → vbdev_split_init/_examine →
 *     spdk_bdev_part_base_construct_ext() → spdk_bdev_part_construct() →
 *     spdk_bdev_register().
 */
int create_vbdev_split(const char *base_bdev_name, unsigned split_count, uint64_t split_size_mb);
/* [한국어] 위 주석 참고 — split 생성 진입점. */

/**
 * Remove all created split bdevs and split config.
 *
 * \param base_bdev_name base bdev name
 * \return 0 on success or negative errno value.
 */
/*
 * [한국어]
 * vbdev_split_destruct - base bdev에 묶인 모든 split vbdev와 split 설정을 제거.
 *
 * @base_bdev_name: 제거 대상 base bdev 이름. split 설정이 없으면 -ENODEV.
 * @return: 0 — 모든 정리가 시작됨, 음수 — errno.
 *
 * 동기/배경: 분할 vbdev들은 part_base 한 개에 묶여 있어, 개별 삭제가 아니라 base
 * 단위로 일괄 unregister한다. spdk_bdev_part_base_hotremove()가 등록된 모든
 * spdk_bdev_part를 unregister하고, 각각 비동기 완료 후 part_base가 free된다.
 * 실행 컨텍스트: SPDK app 스레드. 실제 unregister는 비동기지만, 이 함수는 dispatch
 * 후 정상 즉시 0을 돌려준다.
 *
 * 호출 체인:
 *   rpc_bdev_split_delete() → [vbdev_split_destruct] →
 *     spdk_bdev_part_base_hotremove() → 각 part의 spdk_bdev_unregister().
 */
int vbdev_split_destruct(const char *base_bdev_name);
/* [한국어] 위 주석 참고 — split 제거 진입점. */

/**
 * Get the spdk_bdev_part_base associated with the given split base_bdev.
 *
 * \param base_bdev Bdev to get the part_base from
 * \return pointer to the associated spdk_bdev_part_base
 * \return NULL if the base_bdev is not being split by the split module
 */
/*
 * [한국어]
 * vbdev_split_get_part_base - base bdev에 연결된 split의 part_base를 인트로스펙트.
 *
 * @base_bdev: 조회할 base spdk_bdev 핸들(spdk_bdev_open_ext 후 얻은 spdk_bdev*).
 * @return: 비-NULL — split의 spdk_bdev_part_base, NULL — 해당 base가 split 모듈에
 *          속하지 않음(다른 vbdev이거나 split이 아직 등록 안 된 경우).
 *
 * 동기/배경: 'bdev_split_create' RPC가 응답으로 새로 만들어진 파티션 이름 목록을
 * 돌려주려면 part_base에 연결된 part 리스트(bdev_part_tailq)를 순회해야 한다.
 * 본 함수는 split 모듈 내부의 splits 리스트를 base_bdev로 매칭하여 part_base를
 * 반환하는 lookup 헬퍼다. 외부에서는 읽기 전용으로 사용해야 하며, 내부 큐
 * 구조는 spdk_bdev_part_base_get_tailq()로 별도 접근.
 * 실행 컨텍스트: SPDK app 스레드(주로 RPC). 락 없이 호출하지만, split 등록/해제와
 * 동일 스레드에서만 안전하게 호출되어야 한다(SPDK는 reactor 단위 스레드 고정 모델).
 *
 * 호출 체인:
 *   rpc_bdev_split_create() → spdk_bdev_open_ext() → spdk_bdev_desc_get_bdev() →
 *     [vbdev_split_get_part_base] → spdk_bdev_part_base_get_tailq() → TAILQ_FOREACH.
 */
struct spdk_bdev_part_base *vbdev_split_get_part_base(struct spdk_bdev *base_bdev);
/* [한국어] 위 주석 참고 — 인트로스펙션용 lookup. */

#endif /* SPDK_VBDEV_SPLIT_H */
/* [한국어] 헤더 가드 종료. */
