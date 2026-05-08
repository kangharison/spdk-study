/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] FTL(Flash Translation Layer) 공용 유틸리티 일괄 포함 헤더 (ftl_utils.h)
 *
 * === 파일의 역할 ===
 * lib/ftl/utils/ 아래에 흩어져 있는 다섯 개의 보조 모듈 헤더를 한 번에 끌어들이는
 * "umbrella header"이다. ftl_core/ftl_band/ftl_nv_cache 등 FTL 메인 코드에서
 * 자주 함께 쓰이는 유틸 인터페이스(상수/메모리풀/구성/메타데이터 I/O/속성 시스템)를
 * 일일이 include하지 않고 이 한 줄로 가져올 수 있게 해주는 편의 헤더이다.
 * 이 파일 자체는 어떤 함수/구조체도 선언하지 않으며, 단순한 include 모음에 불과하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK FTL 서브시스템(lib/ftl/)에서, 사용자/관리(mngt) 레이어와 내부 구현 레이어가
 * 공통으로 의존하는 가장 하위의 유틸리티 묶음 위치에 있다.
 * 호출 체인: ftl_core.c / ftl_band.c / ftl_nv_cache.c → ftl_utils.h →
 *   {ftl_defs.h, ftl_mempool.h, ftl_conf.h, ftl_md.h, ftl_property.h}.
 * 실행 컨텍스트: 컴파일 시점에만 의미를 가지며, 런타임에는 존재하지 않는 헤더이다.
 * 호스트 유저스페이스(SPDK reactor 스레드)에서 동작하는 코드들이 사용하는
 * 인터페이스 모음일 뿐, 자체적으로는 어떠한 실행 코드도 포함하지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는(포함하는) 모듈: utils/ftl_defs.h(공통 상수와 ftl_abort/ftl_bug 매크로),
 *   utils/ftl_mempool.h(FTL 전용 DMA 메모리 풀), utils/ftl_conf.h(FTL 디바이스 설정 구조체),
 *   utils/ftl_md.h(메타데이터 영역 I/O 인터페이스),
 *   utils/ftl_property.h(런타임 튜너블 프로퍼티/RPC 시스템).
 * 의존되는 모듈: lib/ftl/ 내부의 거의 모든 .c 파일이 한두 개의 유틸 헤더가 필요할 때
 *   이 단일 헤더로 일괄 포함하므로, FTL 코드 베이스 전반의 빌드 의존성 그래프 꼭짓점에 위치한다.
 * 데이터 흐름 관점에서는 자체 데이터를 보유하지 않으나, 포함하는 헤더들이 정의하는
 * spdk_ftl_dev/ftl_property/ftl_mempool 등의 핵심 자료구조를 호출자에게 노출시키는 통로이다.
 *
 * === 주요 함수/구조체 요약 ===
 * 자체적으로 선언하는 함수/구조체는 없다. 대신 다섯 개의 하위 헤더가 노출하는 인터페이스를
 * 한 번에 사용할 수 있게 해주며, 각 하위 헤더의 역할은 다음과 같다:
 *   - ftl_defs.h: KiB/MiB/GiB/TiB 단위 매크로, ftl_abort()/ftl_bug() 디버그 트랩,
 *     FTL_INVALID_VALUE/FTL_BAND_ID_INVALID 등 "잘못된 값" 표식 상수.
 *   - ftl_mempool.h: DMA 친화적 메모리 풀 ftl_mempool API (NUMA 소켓 지정 가능,
 *     persistent durable-format 객체 ID 변환 지원).
 *   - ftl_conf.h: spdk_ftl_conf (FTL 인스턴스 설정 — 이름, NV cache 옵션 등).
 *   - ftl_md.h: ftl_md (메타데이터 영역의 비동기 read/write/persist API).
 *   - ftl_property.h: ftl_property (RPC로 dump/set 가능한 런타임 튜너블 등록 시스템).
 */

#ifndef FTL_FTL_UTILS_H
#define FTL_FTL_UTILS_H
/* [한국어] 헤더 가드 매크로 - 동일 헤더가 여러 번 #include 되어도
 * 내용이 한 번만 처리되도록 보호한다. SPDK 빌드 시스템에서 .c 파일 하나가
 * 여러 경로로 이 헤더를 끌어들이는 경우가 흔하므로 필수적이다. */

#include "utils/ftl_defs.h"
/* [한국어] FTL 공통 상수/매크로 헤더 — KiB/MiB/GiB/TiB, ftl_abort(), ftl_bug(),
 * FTL_INVALID_VALUE, FTL_BAND_ID_INVALID, FTL_BAND_PHYS_ID_INVALID 매크로 정의를 가져온다.
 * 이 헤더의 매크로는 lib/ftl 전반에서 디버그 트랩과 단위 변환에 사용된다. */
#include "utils/ftl_mempool.h"
/* [한국어] FTL 전용 메모리 풀 인터페이스 — DPDK hugepage 기반의 DMA-친화 메모리 풀로
 * NVMe DMA 버퍼/메타데이터 객체를 생성한다. NUMA 소켓 ID 지정과
 * durable-format 객체 ID(ftl_df_obj_id) 변환을 지원해 슈퍼블록에서 위치를
 * 영속적으로 표현할 수 있게 한다. */
#include "utils/ftl_conf.h"
/* [한국어] FTL 디바이스 설정(spdk_ftl_conf 등) 정의 — bdev 이름, NV cache 사용 여부,
 * verbose 모드 플래그처럼 spdk_ftl_dev_init() 호출 시 사용자가 제공하는 설정값들을
 * 담는다. ftl_property.c가 이 안의 verbose_mode 플래그를 읽는다. */
#include "utils/ftl_md.h"
/* [한국어] FTL 메타데이터(ftl_md) I/O 인터페이스 — 슈퍼블록/L2P/밴드 메타데이터 등
 * 영구 메타데이터를 NV cache나 base bdev에 비동기로 read/write/persist 한다.
 * 모든 작업은 spdk_bdev API 위에 구축되며 SPDK reactor 스레드에서 실행된다. */
#include "utils/ftl_property.h"
/* [한국어] FTL 런타임 튜너블 프로퍼티 시스템 — bdev_ftl_get_properties /
 * bdev_ftl_set_property RPC 핸들러가 사용한다. 등록된 프로퍼티는 JSON으로
 * dump되거나 디코드되어 런타임에 값이 갱신된다. */

#endif /* FTL_FTL_UTILS_H */
/* [한국어] 헤더 가드 종료 표식. 가드 매크로의 짝을 명시적으로 적어 두어
 * 거대 빌드 트리에서 가드 누락을 시각적으로 즉시 식별할 수 있게 한다. */
