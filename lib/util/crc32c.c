/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] CRC-32C(Castagnoli, 다항식 0x1EDC6F41 / 반사 0x82F63B78)
 *               계산 진입점 (crc32c.c)
 *
 * === 파일의 역할 ===
 * SPDK 곳곳에서 매우 자주 호출되는 CRC-32C(흔히 "iSCSI/NVMe CRC")의
 * `spdk_crc32c_update()` 진입점을 제공한다. 빌드 시점에 사용 가능한 가속
 * 경로(ISA-L → x86 SSE4.2 → ARMv8 CRC32 → 순수 SW 폴백)를 매크로 분기로
 * 선택한다. 또한 iov 배열 누적과 NVMe 변형(보수→update→보수)도 함께 제공.
 *
 * === 전체 아키텍처에서의 위치 ===
 * CRC-32C는 SPDK가 다루는 거의 모든 무결성 경로의 표준 다이제스트이다:
 *   - **NVMe-oF TCP/RDMA**: Header Digest(HDGST) 및 Data Digest(DDGST) —
 *     transport 헤더/페이로드 무결성. NVMe-TCP 스펙(NVMe-oF Specification,
 *     TCP transport binding) 정의.
 *   - **NVMe DIF/DIX**: Protection Information(PI)의 Guard 필드(=PI Type 1/2/3)
 *     계산 시 일부 모드. (참고: 일부 PI 변형은 CRC-16 T10 사용 — crc16.c.)
 *   - **iSCSI**: iSCSI Header/Data Digest.
 *   - **bdev DIF 헬퍼(lib/util/dif.c)**가 본 함수를 직접 호출.
 * 호출 흐름 예: NVMe-TCP 송신 경로 → 헤더 직렬화 → spdk_crc32c_update →
 *   PDU 헤더의 HDGST 필드에 결과 기록.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `util_internal.h`(다항식 매크로, table 구조체),
 *   `crc_internal.h`(SPDK_HAVE_* 매크로 정의, intrinsic 헤더 인클루드),
 *   `spdk/crc32.h`(공개 prototype). ISA-L 빌드 시 isa-l/crc.h, x86 빌드 시
 *   x86intrin.h, ARM 빌드 시 arm_acle.h.
 * - 호출자: lib/nvme(NVMe-oF transport), lib/nvmf(target), lib/iscsi,
 *   lib/util/dif.c, module/bdev/* 등 거의 모든 데이터 경로.
 * - 공유 상태: SW 폴백 빌드에서만 g_crc32c_table(constructor 채움, read-only).
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_crc32c_update(buf, len, crc): 단일 버퍼 CRC-32C 갱신. 빌드 분기:
 *     · ISA-L: crc32_iscsi(어셈블리 가속)
 *     · SSE4.2: _mm_crc32_u8/u64 intrinsic
 *     · ARMv8 CRC: __crc32cb/__crc32cd
 *     · SW: 일반 lookup table.
 *   각 분기 모두 8B 정렬 head/mid/tail 패턴으로 unaligned 로드 회피.
 * - spdk_crc32c_iov_update(iov, iovcnt, crc): iov 배열을 순회하며 누적.
 *   NVMe-oF SGL/iov 송수신에서 사용.
 * - spdk_crc32c_nvme(buf, len, crc): NVMe 표준이 요구하는 "초기 보수 +
 *   최종 보수" 변형. NVMe Base Spec의 PI/CRC 처리에 직접 매치.
 */

#include "util_internal.h"
/* [한국어] SPDK_CRC32C_POLYNOMIAL_REFLECT(0x82F63B78) 매크로와
 * struct spdk_crc32_table, crc32_table_init/crc32_update prototype. */
#include "crc_internal.h"
/* [한국어] SPDK_HAVE_ISAL / SPDK_HAVE_SSE4_2 / SPDK_HAVE_ARM_CRC 매크로와
 * 그에 따른 intrinsic 헤더 자동 인클루드. */
#include "spdk/crc32.h"
/* [한국어] 공개 prototype: spdk_crc32c_update / iov / nvme 변형. */

#ifdef SPDK_HAVE_ISAL
/* [한국어] Intel ISA-L(Storage Acceleration Library) 활성 빌드.
 * 어셈블리로 손튜닝된 CRC32C 구현(crc32_iscsi)을 사용 — 가장 빠른 경로. */

/*
 * [한국어]
 * spdk_crc32c_update (ISA-L 가속판)
 *
 * @buf, @len, @crc: 입력 버퍼/길이/누적 CRC.
 * @return: 갱신된 CRC.
 *
 * 호출 체인: 어떤 SPDK 모듈 → spdk_crc32c_update → ISA-L crc32_iscsi.
 * ISA-L crc32_iscsi는 PCLMULQDQ + folding 알고리즘으로 64B 단위 처리.
 */
uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	return crc32_iscsi((unsigned char *)buf, len, crc);
	/* [한국어] ISA-L의 가속 함수에 위임. 이름이 "iscsi"이지만 다항식이
	 * 동일(Castagnoli)해서 NVMe/iSCSI 모두에 사용 가능. */
}

#elif defined(SPDK_HAVE_SSE4_2)
/* [한국어] x86 SSE4.2 CRC32 명령(_mm_crc32_u*) 활성 빌드. ISA-L이 없을 때 사용. */

/*
 * [한국어]
 * spdk_crc32c_update (SSE4.2 intrinsic판)
 *
 * @buf, @len, @crc: 입력 버퍼/길이/누적 CRC.
 * @return: 갱신된 CRC.
 *
 * Intel CRC32 명령(=Nehalem 이후 모든 x86)을 사용. 64B 정렬은 안 하지만
 * 8B 정렬은 head/tail 분리로 보장 → unaligned 로드 회피.
 */
uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	size_t count_pre, count_post, count_mid;
	/* [한국어] head/mid/tail 단계의 처리 바이트 수. */
	const uint64_t *dword_buf;
	/* [한국어] 정렬된 64비트 워드 포인터. */
	uint64_t crc_tmp64;
	/* [한국어] _mm_crc32_u64는 64비트 입출력을 받는 intrinsic이라 임시 64비트
	 * 변수로 확장. */

	/* process the head and tail bytes separately to make the buf address
	 * passed to _mm_crc32_u64 is 8 byte aligned. This can avoid unaligned loads.
	 */
	count_pre = ((uint64_t)buf & 7) == 0 ? 0 : 8 - ((uint64_t)buf & 7);
	/* [한국어] head: 8B 경계까지 부족한 바이트 수. */
	count_post = (uint64_t)((uintptr_t)buf + len) & 7;
	/* [한국어] tail: 끝 주소의 8B 잔여. uintptr_t로 우회해 포인터 산술 경고 회피. */
	count_mid = (len - count_pre - count_post) / 8;
	/* [한국어] 가운데 8B 워드 수. */

	while (count_pre--) {
		crc = _mm_crc32_u8(crc, *(const uint8_t *)buf);
		/* [한국어] _mm_crc32_u8: SSE4.2 CRC32 명령(1B 입력판). 한 명령으로
		 * (crc, byte) → 새 crc. */
		buf = (uint8_t *)buf + 1;
	}

	/* _mm_crc32_u64() needs a 64-bit intermediate value */
	crc_tmp64 = crc;
	/* [한국어] intrinsic 시그니처 호환을 위해 64비트로 확장. */
	dword_buf = (const uint64_t *)buf;
	/* [한국어] 정렬된 buf를 64비트 별칭으로 캐스팅. */

	while (count_mid--) {
		crc_tmp64 = _mm_crc32_u64(crc_tmp64, *dword_buf);
		/* [한국어] _mm_crc32_u64: 한 명령으로 8B를 처리(SSE4.2 64비트판).
		 * 핫패스의 처리량을 가장 끌어올리는 단계. */
		dword_buf++;
	}

	buf = dword_buf;
	/* [한국어] 가운데 처리 후 위치를 byte 포인터로 다시 가져옴. */
	crc = (uint32_t)crc_tmp64;
	/* [한국어] 64비트 임시값에서 하위 32비트만 회수(상위는 0으로 유지됨). */
	while (count_post--) {
		crc = _mm_crc32_u8(crc, *(const uint8_t *)buf);
		/* [한국어] tail 잔여를 1B 단위 명령으로 마저 처리. */
		buf = (uint8_t *)buf + 1;
	}

	return crc;
	/* [한국어] head+mid+tail 누적 결과. */
}

#elif defined(SPDK_HAVE_ARM_CRC)
/* [한국어] ARMv8 CRC32 명령(__crc32cb/__crc32cd) 활성 빌드. */

/*
 * [한국어]
 * spdk_crc32c_update (ARM CRC32 intrinsic판)
 *
 * AArch64 +crc 확장의 명령을 사용. 8B 정렬 패턴은 SSE 경로와 동일.
 */
uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	size_t count_pre, count_post, count_mid;
	/* [한국어] 정렬 단계별 카운트. */
	const uint64_t *dword_buf;
	/* [한국어] 64비트 워드 포인터. */

	/* process the head and tail bytes separately to make the buf address
	 * passed to crc32_cd is 8 byte aligned. This can avoid unaligned loads.
	 */
	count_pre = ((uint64_t)buf & 7) == 0 ? 0 : 8 - ((uint64_t)buf & 7);
	/* [한국어] head 정렬 카운트. */
	count_post = (uint64_t)(buf + len) & 7;
	/* [한국어] tail 잔여. */
	count_mid = (len - count_pre - count_post) / 8;
	/* [한국어] 가운데 8B 워드 수. */

	while (count_pre--) {
		crc = __crc32cb(crc, *(const uint8_t *)buf);
		/* [한국어] __crc32cb: ARMv8 CRC32-Castagnoli byte 명령. */
		buf++;
	}

	dword_buf = (const uint64_t *)buf;
	/* [한국어] 정렬 보장된 후 64비트 포인터로 별칭. */
	while (count_mid--) {
		crc = __crc32cd(crc, *dword_buf);
		/* [한국어] __crc32cd: 8B 입력 ARMv8 CRC32-C 명령. */
		dword_buf++;
	}

	buf = dword_buf;
	/* [한국어] 가운데 후 byte 포인터로 회수. */
	while (count_post--) {
		crc = __crc32cb(crc, *(const uint8_t *)buf);
		/* [한국어] tail 1B 처리. */
		buf++;
	}

	return crc;
	/* [한국어] 누적 CRC 반환. */
}

#else /* Neither SSE 4.2 nor ARM CRC32 instructions available */
/* [한국어] 가속 명령이 없는 환경(예: 일부 ARM32, RISC-V 등)에서의 SW 폴백.
 * 256-엔트리 lookup table을 한 번 만들어두고 byte-by-byte 갱신. */

static struct spdk_crc32_table g_crc32c_table;
/* [한국어] CRC-32C SW 폴백 lookup table. SW 빌드 한정 전역.
 * 설정자: 아래 crc32c_init 생성자.
 * 읽는 자: spdk_crc32c_update 한 곳.
 * 동기화: constructor 이후 read-only → lockless. */

/*
 * [한국어]
 * crc32c_init - 프로세스 시작 시 CRC-32C lookup table 초기화.
 *
 * `__attribute__((constructor))`. main() 진입 전 단일 스레드에서 1회 실행.
 */
__attribute__((constructor)) static void
crc32c_init(void)
{
	crc32_table_init(&g_crc32c_table, SPDK_CRC32C_POLYNOMIAL_REFLECT);
	/* [한국어] Castagnoli 다항식(반사 0x82F63B78)으로 256개 엔트리 채움.
	 * iSCSI RFC 3385, NVMe Base Spec(PI/Digest 절)이 채택한 CRC-32C 정의와
	 * 일치한다. */
}

/*
 * [한국어]
 * spdk_crc32c_update (SW 폴백판)
 *
 * 가속 경로가 없을 때만 컴파일됨. 일반 lookup table 알고리즘을 crc32.c의
 * crc32_update에 위임.
 */
uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	return crc32_update(&g_crc32c_table, buf, len, crc);
	/* [한국어] IEEE 32와 동일한 코어 루프 공유. 다항식만 g_crc32c_table에서
	 * 다르게 들어가 있다. */
}

#endif

/*
 * [한국어]
 * spdk_crc32c_iov_update - 분산된 iovec 배열에 대해 CRC-32C 누적.
 *
 * @iov: scatter-gather 배열 (각 항목은 base/len).
 * @iovcnt: 항목 수.
 * @crc32c: 입력 누적 CRC.
 * @return: 모든 iov를 순서대로 누적한 결과.
 *
 * 동기: NVMe-oF/iSCSI/소켓 송수신에서 데이터는 흔히 헤더+페이로드+패딩 등
 * 여러 iov로 분할되어 있다. 한 호출로 일괄 누적할 수 있게 wrapping한 헬퍼.
 *
 * 실행 컨텍스트: I/O 핫패스에서 직접 호출되므로 가속 경로(ISA-L 등)가 활성
 * 빌드에서 가장 큰 효과. iov가 NULL이면 입력 그대로 반환(no-op).
 *
 * 호출 체인:
 *   transport 송수신 코드 → spdk_crc32c_iov_update → spdk_crc32c_update(N회).
 */
uint32_t
spdk_crc32c_iov_update(struct iovec *iov, int iovcnt, uint32_t crc32c)
{
	int i;
	/* [한국어] iov 인덱스. */

	if (iov == NULL) {
		/* [한국어] NULL은 빈 입력으로 간주하고 입력 CRC 그대로 반환. */
		return crc32c;
	}

	for (i = 0; i < iovcnt; i++) {
		assert(iov[i].iov_base != NULL);
		/* [한국어] 각 iov는 유효 base/len을 가져야 한다는 호출 계약을 명시.
		 * 디버그 빌드에서만 검사. */
		assert(iov[i].iov_len != 0);
		/* [한국어] 0 길이 항목은 비효율 + 버그 시그니처라 금지. */
		crc32c = spdk_crc32c_update(iov[i].iov_base, iov[i].iov_len, crc32c);
		/* [한국어] 단일 버퍼 갱신을 누적. 가속 경로 활성 시 한 번에 큰
		 * iov를 처리. */
	}

	return crc32c;
	/* [한국어] 전 iov 누적 결과. */
}

/*
 * [한국어]
 * spdk_crc32c_nvme - NVMe 표준이 요구하는 CRC-32C 변형 계산.
 *
 * @buf, @len, @crc: 일반 update와 같은 인자.
 * @return: 변형된 CRC.
 *
 * 동기: NVMe 일부 다이제스트는 "초기 보수 + 최종 보수" 컨벤션을 채택한다
 * (NVMe Base Spec의 Protection Information / E2E 데이터 무결성 정의).
 * 호출자가 보수를 직접 다루기 번거롭기 때문에 한 줄 헬퍼로 캡슐화.
 *
 * 호출 체인:
 *   lib/util/dif.c, lib/nvme 모듈 → spdk_crc32c_nvme → spdk_crc32c_update.
 */
uint32_t
spdk_crc32c_nvme(const void *buf, size_t len, uint32_t crc)
{
	return ~(spdk_crc32c_update(buf, len, ~crc));
	/* [한국어] 입력 crc의 보수를 update에 전달하고, 결과의 보수를 다시
	 * 취해 반환. NVMe 사양이 요구하는 "complement on input/output" 패턴. */
}
