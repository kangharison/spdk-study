/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * GUID Partition Table (GPT) specification definitions
 */

/*
 * [한국어 설명] GUID Partition Table(GPT) 스펙 헤더 (gpt_spec.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 UEFI(Unified Extensible Firmware Interface) 표준이 정의한 GPT(GUID Partition Table)
 * 디스크 파티션 레이아웃의 온디스크(on-disk) 자료구조를 SPDK 코드에서 그대로 사용할 수 있도록
 * C 구조체로 표현한다. 또한 GPT 이전 BIOS 시대의 레거시 MBR(Master Boot Record) 자료구조도
 * 함께 정의하여, GPT 디스크 LBA 0 에 위치한 "보호 MBR(Protective MBR)" 을 파싱할 수 있게 한다.
 * 모든 구조체는 디스크에 저장된 바이트 배치를 그대로 반영해야 하므로 `#pragma pack(push,1)` 으로
 * 패딩이 제거되어 있고, 각 구조체 정의 직후 `SPDK_STATIC_ASSERT` 로 sizeof 가 스펙 값과 일치함을
 * 컴파일 타임에 검증한다(MBR=512B, GPT header=92B, GPT partition entry=128B).
 * 본 파일에는 동작 코드가 전혀 없으며, 순수 데이터 정의와 well-known GUID/매크로 상수만 담는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 의 블록 디바이스 추상화 계층(bdev) 중 파티션 처리 모듈인 `lib/bdev/part.c` 와
 * 그 위에 얹히는 `module/bdev/gpt/` 모듈이 디스크의 LBA 0/1 영역을 읽어 이 헤더의 구조체로
 * 캐스팅하여 보호 MBR과 GPT primary header 를 파싱한다. 또한 GPT 백업 사본은 디스크의
 * 마지막 LBA 에 존재하므로, 디스크 size 정보를 얻은 뒤 마지막 섹터를 다시 읽어 동일 구조체로
 * 검증한다. 즉 본 헤더는 "디스크 → bdev I/O → 메모리 → 구조체 cast → 검증" 흐름의
 * **메모리-구조체 매핑 단계**에서 사용되며, NVMe/AIO/Malloc 등 어떤 백엔드를 쓰더라도
 * 동일하게 적용된다(파티션 인식은 백엔드 독립적).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `spdk/stdinc.h`(uint8/16/32/64_t 정의), `spdk/assert.h`(SPDK_STATIC_ASSERT 매크로).
 * - 의존받음: `module/bdev/gpt/gpt.c`/`gpt.h` 가 본 헤더의 `struct spdk_mbr`,
 *   `struct spdk_gpt_header`, `struct spdk_gpt_partition_entry`, `SPDK_GPT_SIGNATURE`,
 *   `SPDK_GPT_PART_TYPE_*` 매크로를 직접 참조하여 GPT bdev 를 파티션 단위로 분할한다.
 * - 데이터 흐름: 디스크 매체(NVMe/SATA/AIO 백엔드) →[bdev_io read]→ 호스트 버퍼 →
 *   `(struct spdk_mbr *)buf` 캐스팅 →[signature/type 검사]→ GPT primary 읽기 →
 *   `(struct spdk_gpt_header *)buf` →[CRC32 검증, my_lba/alternate_lba 정합성]→
 *   partition entry 배열 읽기 → 각 엔트리의 part_type_guid 가 UNUSED GUID 가 아니면
 *   해당 LBA 범위를 새 bdev part 로 등록.
 * - 공유 자료구조: `struct spdk_gpt_guid` (16B raw GUID) — 본 헤더에서 정의되며
 *   파티션 타입/유일 ID/디스크 ID 필드 모두에서 동일 타입으로 재사용된다.
 *
 * === 주요 함수/구조체 요약 ===
 * 본 헤더는 함수가 없고 자료구조와 매크로만 정의한다. 핵심 항목:
 *  - `struct spdk_mbr_chs` (3B)        : MBR 파티션의 CHS(Cylinder/Head/Sector) 주소.
 *  - `struct spdk_mbr_partition_entry` (16B) : MBR 의 4 개 파티션 엔트리 중 하나.
 *  - `struct spdk_mbr` (512B)          : LBA 0 의 마스터 부트 레코드 — GPT 디스크에서는
 *                                       "Protective MBR" 형태로 OS type 0xEE 단일 엔트리.
 *  - `struct spdk_gpt_guid` (16B)      : UEFI 사양의 GUID 원시 바이트 표현.
 *  - `struct spdk_gpt_header` (92B)    : GPT primary/backup 헤더 — signature("EFI PART"),
 *                                       revision, header_crc32, my_lba/alternate_lba,
 *                                       partition_entry_lba, num_partition_entries 등.
 *  - `struct spdk_gpt_partition_entry` (128B) : 파티션 1 개의 메타데이터 — 타입 GUID,
 *                                       유일 GUID, 시작/끝 LBA, attribute 비트필드, UTF-16 이름.
 *  - 매크로 `SPDK_GPT_GUID(...)`        : GUID 의 mixed-endian(첫 3 필드 LE, 마지막 2 필드 BE)
 *                                       표기를 컴파일 타임에 16B raw 배열로 풀어 주는 헬퍼.
 *  - well-known GUID 상수              : UNUSED, EFI System Partition, Legacy MBR.
 */

#ifndef SPDK_GPT_SPEC_H
/* [한국어] 헤더 가드 — 다중 #include 시 중복 정의를 막는다. SPDK_GPT_SPEC_H 매크로가
 * 한 번 정의된 뒤에는 #endif 까지의 본문이 두 번째 #include 부터 모두 건너뛰어진다. */
#define SPDK_GPT_SPEC_H

#include "spdk/stdinc.h"
/* [한국어] uint8_t/uint16_t/uint32_t/uint64_t 등 고정 크기 정수 타입을 끌어온다.
 * 이 파일의 모든 구조체는 디스크 바이트 레이아웃을 정확히 표현해야 하므로 폭이 명시된
 * 정수 타입만 사용해야 한다(plain int 는 플랫폼별로 16/32/64 비트가 달라질 수 있음). */

#include "spdk/assert.h"
/* [한국어] SPDK_STATIC_ASSERT 매크로 정의를 가져온다. C11 의 _Static_assert 또는
 * 그 이전 컴파일러에서는 음수 배열 크기 트릭으로 구현되며, 본 파일에서는 sizeof(struct ...)
 * 가 GPT 스펙이 정한 정확한 바이트 수와 일치하는지 컴파일 타임에 검증하는 데 쓴다. */

#ifdef __cplusplus
/* [한국어] C++ 컴파일러로 이 헤더를 포함할 때, 아래 선언들에 C 링크(name mangling 없음)를
 * 적용하기 위한 가드. SPDK 는 C 라이브러리이지만 C++ 사용자도 include 할 수 있다. */
extern "C" {
#endif

#pragma pack(push, 1)
/* [한국어] 이 시점부터 정의되는 모든 구조체에 대해 멤버 정렬(alignment) 패딩을 1 바이트로
 * 강제하고, 기존 패킹 설정은 스택에 push 한다. GPT/MBR 자료구조는 디스크에 저장되는
 * 온디스크 형식 그대로 메모리에 매핑되어야 하며, 컴파일러가 자연 정렬을 위해 임의 패딩을
 * 끼워 넣으면 디스크에서 읽은 바이트와 구조체 필드 오프셋이 어긋난다 — 이를 막는다. */

#define SPDK_MBR_SIGNATURE 0xAA55
/* [한국어] MBR 의 마지막 2 바이트(오프셋 510~511)에 반드시 들어가야 하는 매직 시그니처.
 * 디스크의 LBA 0 마지막 두 바이트가 0x55, 0xAA(little-endian 으로 읽으면 0xAA55) 가
 * 아니면 그 디스크는 유효한 MBR/GPT 디스크가 아니다. PC BIOS 부트 시대부터 이어진 관습. */

#define SPDK_MBR_OS_TYPE_GPT_PROTECTIVE		0xEE
/* [한국어] MBR partition entry 의 os_type 필드 값 0xEE — "보호 MBR(Protective MBR)" 의
 * 단일 파티션이 디스크 전체를 덮고 있음을 의미한다. UEFI 스펙 5.3.2 Protective MBR 절 참조.
 * GPT 디스크는 LBA 0 에 보호 MBR 을 두어 GPT 를 모르는 옛 도구가 디스크를 "비어있는 것"
 * 으로 오인해 덮어쓰지 못하도록 막는다(전체가 한 파티션으로 점유된 듯 보이게 위장). */
#define SPDK_MBR_OS_TYPE_EFI_SYSTEM_PARTITION	0xEF
/* [한국어] os_type 0xEF — "EFI System Partition" 식별자(MBR 측). UEFI 펌웨어가 부팅 시
 * 로더(.efi)를 찾는 FAT 파티션으로 표시될 때 사용한다. GPT 측에서는
 * SPDK_GPT_PART_TYPE_EFI_SYSTEM_PARTITION GUID 가 같은 의미를 담는다. */

struct spdk_mbr_chs {
	/* [한국어] MBR 파티션 시작/끝 위치를 나타내는 CHS(Cylinder/Head/Sector) 3 바이트.
	 * 현대 디스크는 LBA(Logical Block Addressing)를 쓰므로 CHS 는 호환성용으로만 남았고
	 * GPT 보호 MBR 에서는 통상 0x000200(=CHS 0/0/2) 또는 0xFFFFFE 가 채워진다.
	 * UEFI 스펙 5.2.1 Legacy MBR 절 표 5-1 의 비트 패킹을 그대로 따른다. */

	uint8_t head;
	/* [한국어] Head 번호(0~255). 디스크 헤드(플래터의 한 면)를 가리키던 필드.
	 * 설정자: 디스크 포맷터(파티션 도구). 읽는 자: 본 모듈은 통상 무시(LBA 만 사용).
	 * 값 범위: 0~255. 동기화: 디스크 한 번 쓰기 후 읽기 전용이므로 락 없음. */

	uint16_t sector : 6;
	/* [한국어] 16 비트 워드의 하위 6 비트가 Sector 번호(1~63). 0 은 무효값.
	 * 비트필드 : 6 으로 컴파일러에게 6 비트만 차지함을 알린다.
	 * 설정자: 디스크 포맷터. 읽는 자: 본 모듈은 사용하지 않음(레거시).
	 * 값 범위: 1~63. 동기화: 디스크 정적 메타데이터, 락 불필요. */
	uint16_t cylinder : 10;
	/* [한국어] 같은 16 비트 워드의 상위 10 비트가 Cylinder 번호(0~1023).
	 * 큰 디스크는 1023 cylinder 를 넘기므로 보통 0x3FF 로 saturate 되어 표기된다.
	 * 설정자/읽는 자: 위 sector 와 동일. 값 범위: 0~1023.
	 * 동기화: 정적 메타데이터, 락 불필요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_mbr_chs) == 3, "size incorrect");
/* [한국어] CHS 구조체는 8(head) + 16(sector|cylinder) = 24 비트 = 3 바이트여야 한다.
 * 비트필드 packing 이나 컴파일러 옵션 차이로 4 바이트가 되면 디스크 오프셋이 깨지므로
 * 컴파일 타임에 강제 검증한다. */

struct spdk_mbr_partition_entry {
	/* [한국어] MBR 의 파티션 엔트리(16 B). MBR 은 최대 4 개의 primary partition 을
	 * 가질 수 있으며 각 엔트리가 이 구조체이다. UEFI 스펙 5.2.1 표 5-2 참조.
	 * 보호 MBR 에서는 이 중 첫 번째 엔트리만 os_type=0xEE, start_lba=1,
	 * size_lba=디스크 끝까지(또는 0xFFFFFFFF clamp) 로 채워지고 나머지 3 개는 0. */

	uint8_t reserved : 7;
	/* [한국어] 첫 바이트의 하위 7 비트는 예약(0). 일부 OS 가 활용한 적이 있으나
	 * 표준상 무시해야 한다. 설정자: 포맷터(0 으로 기록). 읽는 자: 무시.
	 * 값 범위: 0. 동기화: 락 불필요(정적). */
	uint8_t bootable : 1;
	/* [한국어] 첫 바이트의 최상위 비트 — 1 이면 "활성(부팅 가능)" 파티션.
	 * 전통적으로 0x80(활성) 또는 0x00(비활성) 두 값만 의미가 있고 나머지는 invalid.
	 * GPT 디스크의 보호 MBR 에서는 일반적으로 0(비활성)이다.
	 * 설정자: 포맷터/부트 로더 설치기. 읽는 자: BIOS MBR boot code(본 모듈은 미사용).
	 * 동기화: 락 불필요. */

	struct spdk_mbr_chs start_chs;
	/* [한국어] 파티션 시작 위치의 CHS 표현(3 B). 보호 MBR 에서는 0x000200(=0/0/2) 가 표준.
	 * 설정자: 포맷터. 읽는 자: 레거시 BIOS — 본 모듈은 start_lba 만 신뢰한다.
	 * 값 범위: 위 spdk_mbr_chs 정의 참조. 동기화: 락 불필요. */

	uint8_t os_type;
	/* [한국어] 파티션 타입 ID(1 B). SPDK_MBR_OS_TYPE_GPT_PROTECTIVE(0xEE)이면 GPT 디스크의
	 * 보호 MBR, 0xEF 이면 EFI System Partition, 0x83 이면 Linux native, 0x07 이면 NTFS 등.
	 * 설정자: 포맷터. 읽는 자: 본 모듈은 0xEE 검사로 GPT 디스크인지 식별.
	 * 값 범위: 0x00(엔트리 비어있음)~0xFF. 동기화: 락 불필요. */

	struct spdk_mbr_chs end_chs;
	/* [한국어] 파티션 끝 위치의 CHS 표현(3 B). 디스크가 8GiB 를 넘으면 CHS 로 표현 불가하여
	 * 0xFFFFFE 로 saturate 된다. 본 모듈은 무시.
	 * 설정자: 포맷터. 읽는 자: 레거시 BIOS. 동기화: 락 불필요. */

	uint32_t start_lba;
	/* [한국어] 파티션 시작 LBA(little-endian 32 비트). 보호 MBR 에서는 1(LBA 1 = GPT primary
	 * header). 본 모듈이 GPT 디스크인지 검사할 때 start_lba==1 도 함께 확인한다.
	 * 설정자: 포맷터. 읽는 자: bdev_gpt 모듈.
	 * 값 범위: 1 ~ 디스크 마지막 LBA. 동기화: 정적, 락 불필요. */
	uint32_t size_lba;
	/* [한국어] 파티션 크기(LBA 개수, 32 비트). 디스크가 2TiB(=2^32 sector × 512B)를 넘으면
	 * 표현 불가하여 0xFFFFFFFF 로 saturate. 보호 MBR 에서는 (디스크 LBA 개수 - 1) 또는
	 * 0xFFFFFFFF.
	 * 설정자: 포맷터. 읽는 자: bdev_gpt 모듈(검증용).
	 * 값 범위: 0~0xFFFFFFFF. 동기화: 락 불필요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_mbr_partition_entry) == 16, "size incorrect");
/* [한국어] MBR 파티션 엔트리는 정확히 16 바이트여야 한다(스펙 표 5-2). MBR 안에 4 개가
 * 들어가 총 64 바이트(오프셋 446~509)를 차지한다. */

struct spdk_mbr {
	/* [한국어] LBA 0 에 위치하는 마스터 부트 레코드 전체(512 B). UEFI 스펙 5.2 절 표 5-1
	 * "Legacy MBR" 의 바이너리 레이아웃을 그대로 표현한다. GPT 디스크에서는 이 구조체가
	 * "보호 MBR" 형태로 채워져 있고, partitions[0] 의 os_type 이 0xEE 이다. */

	uint8_t boot_code[440];
	/* [한국어] 0~439 바이트 — 레거시 BIOS 부트 코드 영역(x86 16-bit code).
	 * GPT 디스크에서는 통상 0 으로 채워지지만, dual-boot 호환을 위해 작은 stub 가
	 * 들어 있을 수 있다. 본 모듈은 이 영역을 읽기만 하고 해석하지 않는다.
	 * 설정자: 부트 로더 설치 도구. 읽는 자: BIOS(legacy) — SPDK 는 무시. */

	uint32_t disk_signature;
	/* [한국어] 440~443 바이트 — Windows NT 디스크 서명(랜덤한 32 비트 ID).
	 * 윈도우는 이 값으로 디스크를 식별해 드라이브 문자를 매핑한다. 보호 MBR 에서는 0.
	 * 설정자: 윈도우 디스크 관리자. 읽는 자: 윈도우 NT 계열 OS. 동기화: 락 불필요. */

	uint16_t reserved_444;
	/* [한국어] 444~445 바이트 — 예약 영역(0). 일부 BIOS 는 disk timestamp 를 두기도 하나
	 * 표준상 0 이어야 한다. 설정자: 포맷터. 읽는 자: 무시. 값: 0. */

	struct spdk_mbr_partition_entry partitions[4];
	/* [한국어] 446~509 바이트 — 4 개의 primary partition 엔트리(각 16 B × 4 = 64 B).
	 * GPT 보호 MBR 에서는 partitions[0].os_type==0xEE 단 하나만 유효하고 나머지 3 개는 0.
	 * 설정자: 디스크 파티션 도구. 읽는 자: bdev_gpt 모듈이 [0] 만 검사하여 GPT 여부 판단.
	 * 동기화: 디스크 정적 메타데이터, 락 불필요. */

	uint16_t mbr_signature;
	/* [한국어] 510~511 바이트 — 반드시 SPDK_MBR_SIGNATURE(0xAA55) 여야 한다.
	 * 이 값이 다르면 MBR 자체가 손상되었거나 GPT 디스크가 아니다.
	 * 설정자: 포맷터. 읽는 자: bdev_gpt 모듈이 GPT 검출의 첫 관문으로 검사. 값: 0xAA55. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_mbr) == 512, "size incorrect");
/* [한국어] MBR 은 정확히 디스크 1 섹터(전통적 512 B) 크기여야 한다. 4Kn(4 KiB sector) 디스크에서도
 * MBR 구조 자체는 처음 512 바이트만 사용한다(나머지는 0 으로 채움). */

#define SPDK_GPT_SIGNATURE "EFI PART"
/* [한국어] GPT 헤더의 시그니처 — 정확히 8 바이트 ASCII 문자열 "EFI PART"(NUL 종료 없음).
 * GPT primary/backup 헤더 모두의 첫 8 바이트가 이 값이어야 유효한 GPT 로 인정된다.
 * UEFI 스펙 5.3.2 표 5-7 GPT Header 의 첫 필드. */

#define SPDK_GPT_REVISION_1_0 0x00010000u
/* [한국어] GPT 헤더 revision 필드 값 — 상위 16 비트 major(=1), 하위 16 비트 minor(=0).
 * 즉 0x00010000 == version 1.0. UEFI 2.x 스펙들이 모두 이 revision 을 사용한다. */

struct spdk_gpt_guid {
	/* [한국어] UEFI/Microsoft 가 사용하는 GUID(Globally Unique Identifier) 16 바이트 표현.
	 * 텍스트 형식 "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE" 에서 앞 3 그룹(A,B,C)은
	 * little-endian 으로, 뒤 2 그룹(D,E)은 big-endian 으로 직렬화되는 mixed-endian 규칙
	 * 때문에 raw 16 바이트로 보관해야 안전하다(컴파일러/엔디안 무관). */

	uint8_t raw[16];
	/* [한국어] GUID 의 16 바이트 원시 표현. SPDK_GPT_GUID(...) 매크로가 이 배열을
	 * 스펙대로 채워 준다. 비교는 memcmp() 로 하며, 분해할 일이 거의 없다.
	 * 설정자: SPDK_GPT_GUID() 매크로 또는 디스크에서 읽은 그대로.
	 * 읽는 자: bdev_gpt 모듈이 partition 의 part_type_guid 와 well-known GUID 를 비교.
	 * 값 범위: 임의 16 바이트(GUID 텍스트 표기와 mixed-endian 매핑).
	 * 동기화: 디스크 정적 데이터, 락 불필요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_gpt_guid) == 16, "size incorrect");
/* [한국어] GUID 는 항상 16 바이트. 구조체 패딩이 끼면 디스크 레이아웃이 어긋나므로
 * 컴파일 타임 검증으로 안전망을 둔다. */

#define SPDK_GPT_GUID(a, b, c, d, e) \
	(struct spdk_gpt_guid){{ \
		(uint8_t)(a), (uint8_t)(((uint32_t)a) >> 8), \
		(uint8_t)(((uint32_t)a) >> 16), (uint8_t)(((uint32_t)a >> 24)), \
		(uint8_t)(b), (uint8_t)(((uint16_t)b) >> 8), \
		(uint8_t)(c), (uint8_t)(((uint16_t)c) >> 8), \
		(uint8_t)(((uint16_t)d) >> 8), (uint8_t)(d), \
		(uint8_t)(((uint64_t)e) >> 40), (uint8_t)(((uint64_t)e) >> 32), (uint8_t)(((uint64_t)e) >> 24), \
		(uint8_t)(((uint64_t)e) >> 16), (uint8_t)(((uint64_t)e) >> 8), (uint8_t)(e) \
	}}
/* [한국어] GUID 텍스트 표기 "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee" 를 컴파일 타임에
 * 16 바이트 raw 배열로 풀어 주는 헬퍼. 핵심은 mixed-endian 규칙이다 —
 *   - a (32 비트): little-endian 4 바이트 (LSB → MSB)
 *   - b (16 비트): little-endian 2 바이트 (LSB → MSB)
 *   - c (16 비트): little-endian 2 바이트 (LSB → MSB)
 *   - d (16 비트): big-endian   2 바이트 (MSB → LSB)
 *   - e (48 비트): big-endian   6 바이트 (MSB → LSB)
 * 위 매크로 본문이 d 와 e 만 ((>>n) → (>>0)) 순서로 큰 자리부터 추출하는 이유.
 * UEFI 스펙 부록 A "GUID and Time Formats" 와 RFC 4122 의 4.1.2 절을 따른다.
 * 호출자는 (struct spdk_gpt_guid) 컴파운드 리터럴로 즉시 초기화에 쓸 수 있다. */

#define SPDK_GPT_PART_TYPE_UNUSED		SPDK_GPT_GUID(0x00000000, 0x0000, 0x0000, 0x0000, 0x000000000000)
/* [한국어] 모든 비트가 0 인 NULL GUID — 파티션 엔트리가 비어있음(미사용)을 의미한다.
 * GPT 헤더의 num_partition_entries 만큼 엔트리 배열을 순회하다가 part_type_guid 가
 * 이 값이면 해당 슬롯은 건너뛴다. UEFI 스펙 5.3.3 절 "Unused Entry". */
#define SPDK_GPT_PART_TYPE_EFI_SYSTEM_PARTITION	SPDK_GPT_GUID(0xC12A7328, 0xF81F, 0x11D2, 0xBA4B, 0x00A0C93EC93B)
/* [한국어] EFI System Partition(ESP) 의 well-known GUID — UEFI 펌웨어가 부팅 시
 * \EFI\BOOT\BOOTX64.EFI 등 OS 로더를 찾는 FAT12/16/32 파티션. UEFI 스펙 표 5-8.
 * MBR 측 SPDK_MBR_OS_TYPE_EFI_SYSTEM_PARTITION(0xEF) 와 의미가 같다. */
#define SPDK_GPT_PART_TYPE_LEGACY_MBR		SPDK_GPT_GUID(0x024DEE41, 0x33E7, 0x11D3, 0x9D69, 0x0008C781F39F)
/* [한국어] "Legacy MBR Partition Scheme" GUID — 파티션 안에 다시 MBR 형식의 하위
 * 파티셔닝이 있음을 표시. 거의 쓰이지 않으나 스펙상 정의되어 있다. UEFI 표 5-8. */

struct spdk_gpt_header {
	/* [한국어] GPT 헤더(92 B). primary 는 LBA 1 에, backup 은 디스크 마지막 LBA 에 위치한다.
	 * 헤더 자신의 my_lba 와 alternate_lba 는 서로의 위치를 가리켜 primary 가 손상되면
	 * backup 으로 복구할 수 있다. UEFI 스펙 5.3.2 표 5-7 의 필드 순서를 그대로 따른다. */

	char gpt_signature[8];
	/* [한국어] 오프셋 0~7 — 반드시 "EFI PART"(SPDK_GPT_SIGNATURE) ASCII 8 바이트.
	 * NUL 종료 없음. 본 모듈은 memcmp(.., "EFI PART", 8) 로 검사한다.
	 * 설정자: 디스크 포맷터. 읽는 자: bdev_gpt 모듈. 값: 고정 8 바이트.
	 * 동기화: 정적 데이터, 락 불필요. */
	uint32_t revision;
	/* [한국어] 오프셋 8~11 — GPT 리비전(상위 16=major, 하위 16=minor). 통상 0x00010000.
	 * 호환성 검사에 사용. 설정자: 포맷터. 읽는 자: bdev_gpt. 값: SPDK_GPT_REVISION_1_0. */
	uint32_t header_size;
	/* [한국어] 오프셋 12~15 — GPT 헤더의 실제 사용 크기(보통 92, 향후 확장 가능).
	 * CRC32 계산 시 이 길이만큼만 해시한다. 설정자: 포맷터. 읽는 자: bdev_gpt(=92 검증). */
	uint32_t header_crc32;
	/* [한국어] 오프셋 16~19 — 헤더의 CRC32. 계산 시 이 필드 자체를 0 으로 두고 header_size
	 * 바이트에 대해 CRC-32(IEEE 802.3 다항식 0x04C11DB7) 를 돌린다.
	 * 설정자: 포맷터(쓰기 시 마지막에 채움). 읽는 자: bdev_gpt 가 헤더 무결성 검증.
	 * 동기화: 헤더가 한 번 쓰인 뒤 갱신은 partition 변경 시에만 발생. */
	uint32_t reserved;
	/* [한국어] 오프셋 20~23 — 0 으로 예약. CRC 계산에 포함된다. 값: 0. */
	uint64_t my_lba;
	/* [한국어] 오프셋 24~31 — 이 헤더가 저장된 LBA. primary=1, backup=disk_last_lba.
	 * 디스크에서 읽은 LBA 와 my_lba 가 일치해야 헤더가 올바른 위치에 있음이 입증된다.
	 * 설정자: 포맷터. 읽는 자: bdev_gpt 가 자기 위치 검증. */
	uint64_t alternate_lba;
	/* [한국어] 오프셋 32~39 — 사본 헤더의 LBA. primary 헤더에는 backup LBA(= last LBA),
	 * backup 헤더에는 primary LBA(=1) 가 들어 있다. 한쪽이 손상되면 alternate_lba 로
	 * 다른 사본을 읽어 복구하는 게 GPT duplication 의 핵심.
	 * 설정자: 포맷터. 읽는 자: bdev_gpt(복구 경로). */
	uint64_t first_usable_lba;
	/* [한국어] 오프셋 40~47 — 사용자 데이터(파티션)에 할당 가능한 첫 LBA. partition entry
	 * 배열이 끝난 직후 LBA 부터 시작한다. 일반적으로 LBA 34(=1+1+32). */
	uint64_t last_usable_lba;
	/* [한국어] 오프셋 48~55 — 사용자 데이터에 할당 가능한 마지막 LBA. backup partition
	 * entry 배열과 backup header 직전까지. 어떤 partition.ending_lba 도 이 값을 넘으면 안 된다. */
	struct spdk_gpt_guid disk_guid;
	/* [한국어] 오프셋 56~71 — 디스크 전체를 식별하는 16 B GUID. 포맷 시 무작위 생성되어
	 * 두 디스크가 동일 GUID 를 가질 확률은 사실상 0. OS 가 디스크를 식별할 때 사용. */
	uint64_t partition_entry_lba;
	/* [한국어] 오프셋 72~79 — partition entry 배열이 시작되는 LBA. primary 헤더에서는 보통 2,
	 * backup 헤더에서는 (last_lba - 32) 부근. */
	uint32_t num_partition_entries;
	/* [한국어] 오프셋 80~83 — partition entry 배열의 길이(개수). 통상 128.
	 * size_of_partition_entry × num_partition_entries 만큼 디스크 영역을 차지한다. */
	uint32_t size_of_partition_entry;
	/* [한국어] 오프셋 84~87 — partition entry 한 개의 크기(바이트). 반드시 128 의 배수,
	 * 현행 스펙에서는 정확히 128. 향후 확장 시 더 커질 수 있어 sizeof(...) 대신 이 필드를 써야 함. */
	uint32_t partition_entry_array_crc32;
	/* [한국어] 오프셋 88~91 — partition entry 배열 전체에 대한 CRC32.
	 * 헤더가 무사해도 entry 배열이 손상되면 backup 으로 복구해야 한다.
	 * 설정자: 포맷터(엔트리 변경 시마다 재계산). 읽는 자: bdev_gpt(엔트리 무결성 검증). */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_gpt_header) == 92, "size incorrect");
/* [한국어] GPT 헤더는 정확히 92 바이트(UEFI 스펙 표 5-7). 나머지 패딩(섹터 끝까지)은 0 이며
 * CRC 에는 포함되지 않는다(header_size=92 만 해시). */

struct spdk_gpt_partition_entry {
	/* [한국어] GPT 파티션 엔트리(128 B). 디스크에는 partition_entry_lba 부터
	 * num_partition_entries(통상 128) 개가 연속으로 저장된다. UEFI 스펙 5.3.3 표 5-9. */

	struct spdk_gpt_guid part_type_guid;
	/* [한국어] 오프셋 0~15 — 파티션 타입 GUID. NULL GUID(SPDK_GPT_PART_TYPE_UNUSED)이면
	 * 이 슬롯은 비어 있고, ESP/Linux/Windows Basic Data 등 well-known GUID 와 비교하여
	 * 파일시스템 종류를 추정한다.
	 * 설정자: 포맷터/파티션 도구. 읽는 자: bdev_gpt 가 UNUSED 여부와 종류 식별에 사용. */
	struct spdk_gpt_guid unique_partition_guid;
	/* [한국어] 오프셋 16~31 — 이 파티션 자체를 유일하게 식별하는 GUID(포맷 시 랜덤 생성).
	 * 동일 디스크 내 두 파티션이 같은 unique GUID 를 가지면 안 된다. OS 마운트 시 라벨/UUID 매핑에 활용. */
	uint64_t starting_lba;
	/* [한국어] 오프셋 32~39 — 파티션 시작 LBA(포함). first_usable_lba ≤ starting_lba 이어야 함.
	 * 설정자: 파티션 도구. 읽는 자: bdev_gpt 가 새 bdev part 등록 시 base_offset 으로 사용. */
	uint64_t ending_lba;
	/* [한국어] 오프셋 40~47 — 파티션 끝 LBA(포함). starting_lba ≤ ending_lba ≤ last_usable_lba.
	 * 파티션 크기 = ending_lba - starting_lba + 1.
	 * 설정자: 파티션 도구. 읽는 자: bdev_gpt 가 part bdev 의 num_blocks 계산에 사용. */
	struct {
		/* [한국어] 오프셋 48~55 — 파티션 attribute 비트필드(64 비트). UEFI 스펙 표 5-10. */

		uint64_t required : 1;
		/* [한국어] 비트 0 — "Required Partition" 플래그. 1 이면 OEM/펌웨어가 반드시 보존해야
		 * 하는 파티션(예: 복구용). 파티션 도구가 마음대로 지우면 안 된다. */
		uint64_t no_block_io_proto : 1;
		/* [한국어] 비트 1 — "No Block IO Protocol". 1 이면 UEFI 펌웨어가 이 파티션에
		 * EFI_BLOCK_IO_PROTOCOL 핸들을 만들지 않는다(블록 디바이스로 노출 안 함). */
		uint64_t legacy_bios_bootable : 1;
		/* [한국어] 비트 2 — "Legacy BIOS Bootable". 1 이면 CSM(Compatibility Support Module)
		 * 환경에서 이 파티션이 부팅 가능. UEFI 네이티브 부팅과는 무관. */
		uint64_t reserved_uefi : 45;
		/* [한국어] 비트 3~47 — UEFI 미래 확장용 예약 영역(반드시 0). */
		uint64_t guid_specific : 16;
		/* [한국어] 비트 48~63 — part_type_guid 별로 의미가 다른 영역.
		 * 예: Microsoft Basic Data Partition GUID 일 때 비트 60=read-only, 비트 62=hidden 등.
		 * 설정자/읽는 자: 해당 OS/파일시스템 드라이버. SPDK 는 통상 무시. */
	} attr;
	uint16_t partition_name[36];
	/* [한국어] 오프셋 56~127 — 파티션 이름(UTF-16LE, 최대 36 코드 유닛 = 72 B).
	 * 사람이 보기 위한 라벨이며 NUL(0x0000) 로 종료될 수 있다. UEFI 가 정의한 길이는
	 * 정확히 72 바이트이고 그 이상은 잘린다.
	 * 설정자: 파티션 도구가 사용자 입력으로 채움. 읽는 자: 디스플레이 용도, SPDK 는 옵션 사용.
	 * 동기화: 정적 데이터, 락 불필요. */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_gpt_partition_entry) == 128, "size incorrect");
/* [한국어] partition entry 는 현재 스펙상 정확히 128 바이트. GPT 헤더의
 * size_of_partition_entry 가 128 의 배수일 수 있어 향후 확장 가능하지만, 본 구조체는
 * 128 B 고정으로 가정하고 검증한다(SPDK 가 지원하는 범위). */

#pragma pack(pop)
/* [한국어] 파일 상단의 #pragma pack(push, 1) 을 되돌린다. 이후 다른 헤더에서 정의되는
 * 구조체는 컴파일러 기본 정렬로 돌아간다. push/pop 짝을 반드시 맞춰야 다른 헤더에 영향이 없다. */

#ifdef __cplusplus
}
/* [한국어] extern "C" { 블록 종료. C++ 컴파일러에서만 활성화. */
#endif

#endif
/* [한국어] SPDK_GPT_SPEC_H 헤더 가드 종료. */
