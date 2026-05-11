/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright(c) ARM Limited. 2021 All rights reserved.
 */

/*
 * [한국어 설명] ARMv8 SVE(Scalable Vector Extension) 기반 base64 인코더/디코더 (base64_sve.c)
 *
 * === 파일의 역할 ===
 * ARMv8-A의 SVE/SVE2 가변 길이 SIMD를 사용하여 base64 인코드/디코드를 수행한다.
 * NEON과 달리 SVE는 벡터 길이(VL)가 128/256/.../2048 비트 사이에서 구현 의존적으로 결정되므로
 * 이 파일은 svcntb()로 런타임 VL을 조회한 뒤 16/32/48/64+ 바이트 등 케이스별 분기로
 * LUT 적재 전략과 룩업 인스트럭션을 다르게 구성한다. 인코드 LUT(64B)와 디코드 LUT(128B)는
 * 한 SVE 레지스터에 들어가지 않을 수 있어 여러 svtbl 룩업 결과를 합산하는 보조 함수
 * (table_lookup_*vec)를 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK util 라이브러리 → base64 가속 백엔드(ARM SVE 전용).
 * 호출 체인:
 *   사용자 코드 → spdk_base64_encode/decode (lib/util/base64.c)
 *     → __aarch64__ + SVE 빌드 시 base64_encode_sve / base64_decode_sve (이 파일)
 *     → 잔여 처리는 base64.c 스칼라 폴백
 * 실행 컨텍스트: 호스트 유저스페이스, SPDK reactor 또는 일반 스레드. 순수 계산.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/stdinc.h, arm_sve.h(SVE 인트린식: svuint8_t, svtbl_u8, svld3_u8 등).
 * - 의존하는 자: lib/util/base64.c가 SVE 가능 빌드에서 이 파일의 함수를 호출.
 * - 데이터 흐름:
 *   인코드: 사용자 입력(8비트 바이트) → svld3_u8로 deinterleave → 비트 시프트로 6비트 4분할 →
 *           VL에 따른 svtbl/table_lookup_*vec 룩업 → svst4_u8로 인터리브 저장.
 *   디코드: ASCII 입력 → svld4_u8 → 128B LUT 룩업(VL에 따라 1~8개 svtbl 결과 합) →
 *           유효성 검사 → 6→8비트 패킹 → svst3_u8 저장.
 *
 * === 주요 함수/구조체 요약 ===
 * - table_lookup_8vec / 4vec / 3vec / 2vec : LUT를 여러 SVE 벡터로 분할 적재한 경우의 룩업 헬퍼.
 *   각 벡터에서 인덱스를 svsub하며 VL을 기준으로 차감, 결과를 합쳐 최종 LUT 값 산출. 0xFF가 있으면 -1.
 * - convert_6bits_to_8bits : 입력 3바이트(24비트)를 6비트 4그룹으로 분해.
 * - convert_8bits_to_6bits : 6비트 4그룹을 8비트 3바이트로 패킹.
 * - base64_encode_sve : VL=16/32/48/64+ 케이스별 메인 인코딩 루프.
 * - base64_decode_sve : VL=16/32/48/64-112/128+ 케이스별 메인 디코딩 루프(검증 포함).
 *
 * === 알고리즘 핵심 ===
 * SVE는 svwhilelt_b8(i, N)으로 만든 술어(predicate)를 사용해 루프 끝의 부분 벡터도
 * 한 번에 처리한다. svtbl_u8은 인덱스가 VL 이상이면 0을 반환하므로, LUT를 여러 벡터로
 * 나누어 인덱스에서 VL씩 차감해가며 svtbl을 누적합산하면 임의 크기 LUT 룩업을 구성할 수 있다.
 */

#ifndef __aarch64__
/* [한국어] aarch64 타깃 외에는 컴파일 중단. */
#error Unsupported hardware
#endif

#include "spdk/stdinc.h"
/* [한국어] uint8_t/size_t 등 표준 타입과 NULL 등 매크로. */
#include <arm_sve.h>
/* [한국어] ARM SVE 인트린식 헤더. svuint8_t, svbool_t, svld1/svtbl/svadd 등 모든
 * SVE/SVE2 인트린식 함수와 가변 길이 벡터 타입 정의를 가져온다. */

/*
 * [한국어]
 * table_lookup_8vec - 128B LUT를 8개 SVE 벡터(VL=16)로 분할 적재한 경우의 디코드 LUT 룩업.
 *
 * @tbl_vec0..7: LUT의 16B 청크 8개. tbl_vec0=[0..15], ..., tbl_vec7=[112..127].
 * @indices:     ASCII 입력 바이트 벡터(0..127 범위).
 * @output:      [out] 디코드된 6비트 값 또는 0xFF(invalid).
 * @p8_in:       유효 레인 술어(루프 끝 잔여 처리에서 일부만 활성).
 * @vl:          현재 SVE 벡터 길이(바이트). 이 분기에서는 16.
 * @return:      0 성공, -1 결과에 0xFF가 있어 invalid 입력 검출.
 *
 * 동작: base64 디코드 표 처음 32바이트(ASCII [0..31])는 모두 invalid(0xFF)이므로
 * tbl_vec0/tbl_vec1 룩업을 생략하고 tbl_vec2부터 시작한다. svtbl_u8은 인덱스가
 * VL 이상이면 0을 반환하므로, 매 단계에서 indices에서 vl만큼 차감하며 룩업하고
 * 결과를 svadd로 누적하면 다중 벡터 LUT 룩업이 완성된다.
 *
 * 호출 체인: base64_decode_sve(VL=16) → [table_lookup_8vec]
 * 실행 컨텍스트: 같은 스레드, 부수 효과 없음.
 */
static int
table_lookup_8vec(svuint8_t tbl_vec0, svuint8_t tbl_vec1, svuint8_t tbl_vec2, svuint8_t tbl_vec3,
		  svuint8_t tbl_vec4, svuint8_t tbl_vec5, svuint8_t tbl_vec6, svuint8_t tbl_vec7,
		  svuint8_t indices, svuint8_t *output, svbool_t p8_in, uint64_t vl)
{
	svuint8_t res2, res3, res4, res5, res6, res7;
	/* [한국어] 6개의 부분 LUT 룩업 결과를 보관할 임시 SVE 벡터들. */

	/*
	 * In base64 decode table, the first 32 elements are invalid value,
	 * so skip tbl_vec0 and tbl_vec1
	 */
	indices = svsub_n_u8_z(p8_in, indices, 2 * vl);
	/* [한국어] indices에서 32(=2*vl)을 빼서 [32..127] 영역을 [0..95]로 옮긴다.
	 * _z 접미사는 비활성 레인을 0으로 처리(zeroing). 차감 결과 음수는 wrap-around로 큰 값 → svtbl이 0 반환. */
	res2 = svtbl_u8(tbl_vec2, indices);
	/* [한국어] 첫 16엔트리(원래 ASCII [32..47]) 룩업. 다른 영역은 0. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 한 단계 더 차감하여 다음 LUT 청크의 인덱스로 정렬. */
	res3 = svtbl_u8(tbl_vec3, indices);
	/* [한국어] 다음 16엔트리(원래 ASCII [48..63]) 룩업. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 다음 청크용 인덱스 정렬. */
	res4 = svtbl_u8(tbl_vec4, indices);
	/* [한국어] ASCII [64..79] 영역 룩업. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 다음 청크. */
	res5 = svtbl_u8(tbl_vec5, indices);
	/* [한국어] ASCII [80..95]. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 다음 청크. */
	res6 = svtbl_u8(tbl_vec6, indices);
	/* [한국어] ASCII [96..111]. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 마지막 청크용 정렬. */
	res7 = svtbl_u8(tbl_vec7, indices);
	/* [한국어] ASCII [112..127] 영역 룩업. */

	*output = svdup_n_u8(0);
	/* [한국어] 누적 합 시작값으로 0벡터 준비. */
	*output = svadd_u8_z(p8_in, res2, *output);
	/* [한국어] tbl_vec2 청크(ASCII [32..47]) 결과 누적. 입력 인덱스마다 정확히 한
	 * 청크에서만 0이 아닌 값이 나오므로 결과는 그 한 값이 된다. */
	*output = svadd_u8_z(p8_in, res3, *output);
	/* [한국어] tbl_vec3 청크(ASCII [48..63]) 결과 누적. */
	*output = svadd_u8_z(p8_in, res4, *output);
	/* [한국어] tbl_vec4 청크(ASCII [64..79]) 결과 누적. */
	*output = svadd_u8_z(p8_in, res5, *output);
	/* [한국어] tbl_vec5 청크(ASCII [80..95]) 결과 누적. */
	*output = svadd_u8_z(p8_in, res6, *output);
	/* [한국어] tbl_vec6 청크(ASCII [96..111]) 결과 누적. */
	*output = svadd_u8_z(p8_in, res7, *output);
	/* [한국어] tbl_vec7 청크(ASCII [112..127]) 결과 누적 → 6개 부분 결과 합산 완료. */

	if (svcntp_b8(p8_in, svcmpeq_n_u8(p8_in, *output, 255))) {
		/* [한국어] 결과에 0xFF가 있는 활성 레인 수. 0이 아니면 invalid base64 문자가 발견된 것.
		 * svcmpeq_n_u8 → 비트마스크 술어, svcntp_b8 → true 레인 개수. */
		return -1;
	}

	return 0;
	/* [한국어] 모든 활성 레인이 유효한 디코드값을 가진다. */
}

/*
 * [한국어]
 * table_lookup_4vec - LUT를 4개 SVE 벡터로 분할 적재한 경우의 인코드/디코드 LUT 룩업.
 *
 * @tbl_vec0: LUT의 [0..vl-1] 영역 (첫 청크).
 * @tbl_vec1: LUT의 [vl..2*vl-1] 영역.
 * @tbl_vec2: LUT의 [2*vl..3*vl-1] 영역.
 * @tbl_vec3: LUT의 [3*vl..4*vl-1] 영역 (마지막 청크).
 * @indices:  인덱스 벡터(인코드: 0..63 6비트값 / 디코드: ASCII 0..127).
 * @output:   [out] 룩업 결과 벡터. 0..63 또는 0xFF(invalid).
 * @p8_in:    유효 레인 술어(루프 잔여 처리에서 일부만 활성).
 * @vl:       현재 SVE 벡터 길이(바이트). 이 헬퍼는 VL=16(64B LUT 4분할 인코드) 또는
 *            VL=32(128B LUT 4분할 디코드)에서 사용.
 * @return:   0 성공, -1 결과에 0xFF(invalid) 검출(디코드 컨텍스트에서만 의미).
 *
 * 동작: svtbl_u8은 인덱스 ≥ vl 이면 0 반환. 매 단계 indices에서 vl 차감하며 다음
 * 청크의 [0..vl-1] 인덱스로 정규화. 4개 결과를 svadd로 누적하면 입력별로 한
 * 청크에서만 nonzero이므로 OR과 같은 효과.
 *
 * 호출 체인:
 *   base64_encode_sve(VL=16) → [table_lookup_4vec]  (인코드 64B LUT 4분할)
 *   base64_decode_sve(VL=32) → [table_lookup_4vec]  (디코드 128B LUT 4분할)
 * 실행 컨텍스트: 같은 스레드 인라인, 부수 효과 없음.
 */
static int
table_lookup_4vec(svuint8_t tbl_vec0, svuint8_t tbl_vec1, svuint8_t tbl_vec2, svuint8_t tbl_vec3,
		  svuint8_t indices, svuint8_t *output, svbool_t p8_in, uint64_t vl)
{
	svuint8_t res0, res1, res2, res3;
	/* [한국어] 4개 부분 LUT 룩업 결과 임시 벡터. */

	res0 = svtbl_u8(tbl_vec0, indices);
	/* [한국어] 첫 16엔트리(또는 VL바이트만큼) 룩업. 인덱스 < vl일 때만 0이 아닌 값. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 다음 청크용 인덱스 정렬: 인덱스에서 vl 차감. */
	res1 = svtbl_u8(tbl_vec1, indices);
	/* [한국어] 두 번째 청크 룩업. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 다음 청크용 정렬. */
	res2 = svtbl_u8(tbl_vec2, indices);
	/* [한국어] 세 번째 청크 룩업. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 다음 청크용 정렬. */
	res3 = svtbl_u8(tbl_vec3, indices);
	/* [한국어] 네 번째 청크 룩업. */

	*output = svdup_n_u8(0);
	/* [한국어] 0벡터로 누적 시작. */

	*output = svadd_u8_z(p8_in, res0, *output);
	/* [한국어] 1번째 청크 결과 누적. 입력별로 한 청크에서만 nonzero이므로 OR과 동일. */
	*output = svadd_u8_z(p8_in, res1, *output);
	/* [한국어] 2번째 청크 결과 누적. */
	*output = svadd_u8_z(p8_in, res2, *output);
	/* [한국어] 3번째 청크 결과 누적. */
	*output = svadd_u8_z(p8_in, res3, *output);
	/* [한국어] 4번째 청크 결과 누적 → 4개 부분 결과 합산 완료. */

	if (svcntp_b8(p8_in, svcmpeq_n_u8(p8_in, *output, 255))) {
		/* [한국어] 결과에 0xFF가 있으면 디코드 시 invalid 입력. */
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * table_lookup_3vec - 128B LUT를 3개 SVE 벡터(VL=48)로 분할 적재한 경우의 디코드 LUT 룩업.
 *
 * @tbl_vec0: LUT의 [0..vl-1] 영역(=[0..47]).
 * @tbl_vec1: LUT의 [vl..2*vl-1] 영역(=[48..95]).
 * @tbl_vec2: LUT의 [2*vl..127] 영역(=[96..127], 마지막 32B만 유효 — 부분 술어로 적재됨).
 * @indices:  ASCII 입력 바이트 벡터(0..127).
 * @output:   [out] 디코드된 6비트 값 또는 0xFF(invalid).
 * @p8_in:    유효 레인 술어(루프 잔여 처리에서 일부만 활성).
 * @vl:       SVE 벡터 길이(바이트). 이 분기에서는 48.
 * @return:   0 성공, -1 결과에 0xFF가 있어 invalid 입력 검출.
 *
 * 동작: 각 청크에서 svtbl_u8은 인덱스 < vl 일 때만 nonzero 반환하고, 그 이상이면 0.
 * 매 단계 indices에서 vl만큼 saturating sub하여 다음 청크의 [0..vl-1] 인덱스로 변환.
 * 결과를 svadd로 누적하면 입력별로 정확히 한 청크에서 nonzero가 나와 OR과 같은 효과.
 *
 * 호출 체인: base64_decode_sve(VL=48 분기) → [table_lookup_3vec]
 * 실행 컨텍스트: 같은 스레드 인라인 호출, 부수 효과 없음.
 */
static int
table_lookup_3vec(svuint8_t tbl_vec0, svuint8_t tbl_vec1, svuint8_t tbl_vec2, svuint8_t indices,
		  svuint8_t *output, svbool_t p8_in, uint64_t vl)
{
	svuint8_t res0, res1, res2;
	/* [한국어] 3개 부분 LUT 룩업 결과. */

	res0 = svtbl_u8(tbl_vec0, indices);
	/* [한국어] 첫 vl 바이트(=48B)에 해당하는 LUT 영역 룩업. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 다음 청크용 정렬. */
	res1 = svtbl_u8(tbl_vec1, indices);
	/* [한국어] 둘째 청크 룩업. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 마지막 청크용 정렬. */
	res2 = svtbl_u8(tbl_vec2, indices);
	/* [한국어] 셋째 청크 룩업(LUT의 [96..127] 부분). */

	*output = svdup_n_u8(0);
	/* [한국어] 0벡터 시작. */

	*output = svadd_u8_z(p8_in, res0, *output);
	/* [한국어] 부분 결과 누적합 시작. */
	*output = svadd_u8_z(p8_in, res1, *output);
	*output = svadd_u8_z(p8_in, res2, *output);
	/* [한국어] 3개 부분 합산 완료. */

	if (svcntp_b8(p8_in, svcmpeq_n_u8(p8_in, *output, 255))) {
		/* [한국어] 결과에 0xFF 있으면 invalid - 호출자에게 -1 반환으로 알림. */
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * table_lookup_2vec - LUT를 2개 SVE 벡터로 분할 적재한 경우의 룩업.
 *
 * @tbl_vec0: LUT의 [0..vl-1] 영역.
 * @tbl_vec1: LUT의 [vl..end-1] 영역(인코드 시 64B 中 두 번째 청크, 디코드 시 128B 中 두 번째 청크).
 * @indices:  인덱스 벡터(인코드: 0..63 6비트값, 디코드: ASCII 0..127).
 * @output:   [out] 룩업 결과 벡터. 디코드 시 0..63 또는 0xFF(invalid).
 * @p8_in:    유효 레인 술어.
 * @vl:       SVE 벡터 길이(바이트).
 * @return:   0 성공, -1 결과에 0xFF(invalid) 검출(디코드 컨텍스트에서만 의미).
 *
 * 사용처:
 *  - 인코드(VL=32 또는 48): 64B LUT를 두 청크로 분할(첫 vl B + 둘째 64-vl B).
 *  - 디코드(VL=64..112): 128B LUT를 두 청크로 분할(첫 vl B + 둘째 128-vl B).
 *
 * 호출 체인:
 *   base64_encode_sve(VL=32|48) → [table_lookup_2vec]
 *   base64_decode_sve(VL=64|80|96|112) → [table_lookup_2vec]
 * 실행 컨텍스트: 같은 스레드 인라인, 부수 효과 없음.
 */
static int
table_lookup_2vec(svuint8_t tbl_vec0, svuint8_t tbl_vec1, svuint8_t indices, svuint8_t *output,
		  svbool_t p8_in, uint64_t vl)
{
	svuint8_t res0, res1;
	/* [한국어] 2개 부분 결과 임시. */

	res0 = svtbl_u8(tbl_vec0, indices);
	/* [한국어] 첫 vl 바이트 LUT 영역 룩업. */
	indices = svsub_n_u8_z(p8_in, indices, vl);
	/* [한국어] 두 번째 청크용 인덱스 정렬. */
	res1 = svtbl_u8(tbl_vec1, indices);
	/* [한국어] 두 번째 청크 룩업. */

	*output = svdup_n_u8(0);
	/* [한국어] 0벡터 시작. */

	*output = svadd_u8_z(p8_in, res0, *output);
	/* [한국어] 부분 결과 누적. */
	*output = svadd_u8_z(p8_in, res1, *output);
	/* [한국어] 두 부분 합산 완료 - 활성 레인마다 한 곳에서만 nonzero이므로 결과는 그 값. */

	if (svcntp_b8(p8_in, svcmpeq_n_u8(p8_in, *output, 255))) {
		/* [한국어] invalid 검출 시 -1. */
		return -1;
	}

	return 0;
}

/*
 * [한국어]
 * convert_6bits_to_8bits - 입력 24비트(3바이트)를 6비트 4그룹으로 분해(인코드 전처리).
 * (함수명은 다소 혼동되지만, 실제로는 8비트 입력 3개를 6비트 4개로 변환)
 *
 * @pred:   유효 레인 술어. 비활성 레인은 zeroing predicate로 0 처리.
 * @src:    원본 8비트 데이터 시작 주소(인코드 입력 바이트 스트림).
 * @temp0:  [out] 첫 번째 6비트 인덱스 벡터(b0의 상위 6비트).
 * @temp1:  [out] 두 번째 6비트 인덱스(b0 하위 2비트 + b1 상위 4비트).
 * @temp2:  [out] 세 번째 6비트 인덱스(b1 하위 4비트 + b2 상위 2비트).
 * @temp3:  [out] 네 번째 6비트 인덱스(b2 하위 6비트).
 * @return: 없음.
 *
 * 동작: SIMD 친화적 svld3_u8가 src의 트리플(3채널 인터리브)을 b0/b1/b2 세 채널로
 * 자동 deinterleave한다. 그 후 NEON 인코더와 동일한 비트 시프트/마스크 패턴으로
 * 24비트를 6비트 4분할하여 LUT 인덱스를 생성한다.
 *
 * 호출 체인: base64_encode_sve → [convert_6bits_to_8bits] → 이후 svtbl/table_lookup_*vec 룩업.
 * 실행 컨텍스트: 같은 스레드 인라인, 부수 효과 없음(메모리 read-only + 스택 변수 갱신).
 */
static inline void
convert_6bits_to_8bits(svbool_t pred, uint8_t *src, svuint8_t *temp0, svuint8_t *temp1,
		       svuint8_t *temp2, svuint8_t *temp3)
{
	svuint8_t str0, str1, str2;
	/* [한국어] deinterleave된 b0/b1/b2 채널을 담을 임시 SVE 벡터. */
	svuint8x3_t ld_enc_input;
	/* [한국어] svld3_u8 결과를 받을 3채널 컴파운드 타입. */

	ld_enc_input = svld3_u8(pred, src);
	/* [한국어] src에서 3채널 인터리브 로드. 활성 레인만 채워지고 비활성 레인은 정의되지 않음. */

	str0 = svget3_u8(ld_enc_input, 0);
	/* [한국어] 채널 0(=b0) 추출. */
	str1 = svget3_u8(ld_enc_input, 1);
	/* [한국어] 채널 1(=b1) 추출. */
	str2 = svget3_u8(ld_enc_input, 2);
	/* [한국어] 채널 2(=b2) 추출. */


	*temp0 = svlsr_n_u8_z(pred, str0, 2);
	/* [한국어] temp0 = b0 >> 2 (상위 6비트). 첫 번째 6비트 인덱스. */
	*temp1 = svand_u8_z(pred, svorr_u8_z(pred, svlsr_n_u8_z(pred, str1, 4), svlsl_n_u8_z(pred, str0,
					     4)),
			    svdup_u8(0x3F));
	/* [한국어] temp1 = ((b0 << 4) | (b1 >> 4)) & 0x3F. 두 번째 6비트 인덱스. */
	*temp2 = svand_u8_z(pred, svorr_u8_z(pred, svlsr_n_u8_z(pred, str2, 6), svlsl_n_u8_z(pred, str1,
					     2)),
			    svdup_u8(0x3F));
	/* [한국어] temp2 = ((b1 << 2) | (b2 >> 6)) & 0x3F. 세 번째 6비트 인덱스. */
	*temp3 = svand_u8_z(pred, str2, svdup_u8(0x3F));
	/* [한국어] temp3 = b2 & 0x3F. 네 번째(마지막) 6비트 인덱스. */
}

/*
 * [한국어]
 * convert_8bits_to_6bits - 6비트 4그룹을 8비트 3바이트로 패킹(디코드 후처리).
 * (함수명은 다소 혼동되지만, 실제로는 6비트 4개 → 8비트 3개로 변환)
 *
 * @pred:    유효 레인 술어.
 * @temp0:   첫 번째 6비트값(0..63) 벡터(LUT 룩업 결과).
 * @temp1:   두 번째 6비트값.
 * @temp2:   세 번째 6비트값.
 * @temp3:   네 번째 6비트값.
 * @output0: [out] 디코드된 첫 번째 8비트 바이트(b0).
 * @output1: [out] 디코드된 두 번째 8비트 바이트(b1).
 * @output2: [out] 디코드된 세 번째 8비트 바이트(b2).
 * @return:  없음.
 *
 * 동작: NEON 디코더와 동일한 비트 시프트/OR 패턴으로 4×6 = 24비트를 3×8 = 24비트로 패킹.
 *  - out0 = (t0 << 2) | (t1 >> 4)
 *  - out1 = (t1 << 4) | (t2 >> 2)
 *  - out2 = (t2 << 6) | t3
 *
 * 호출 체인: base64_decode_sve → [convert_8bits_to_6bits] → svst3_u8로 인터리브 저장.
 * 실행 컨텍스트: 같은 스레드 인라인, 부수 효과 없음.
 */
static inline void
convert_8bits_to_6bits(svbool_t pred, svuint8_t temp0, svuint8_t temp1, svuint8_t temp2,
		       svuint8_t temp3, svuint8_t *output0, svuint8_t *output1, svuint8_t *output2)
{
	*output0 = svorr_u8_z(pred, svlsl_n_u8_z(pred, temp0, 2), svlsr_n_u8_z(pred, temp1, 4));
	/* [한국어] out0 = (t0 << 2) | (t1 >> 4): 첫 번째 디코드된 8비트 바이트. */
	*output1 = svorr_u8_z(pred, svlsl_n_u8_z(pred, temp1, 4), svlsr_n_u8_z(pred, temp2, 2));
	/* [한국어] out1 = (t1 << 4) | (t2 >> 2): 두 번째 8비트 바이트. */
	*output2 = svorr_u8_z(pred, svlsl_n_u8_z(pred, temp2, 6), temp3);
	/* [한국어] out2 = (t2 << 6) | t3: 세 번째 8비트 바이트. */
}

/*
 * [한국어]
 * base64_encode_sve - SVE를 사용해 입력 바이트 → base64 ASCII로 인코딩.
 *
 * @dst:       출력 포인터의 포인터(전진).
 * @enc_table: 64엔트리 인코딩 알파벳(표준 또는 URL-safe).
 * @src:       입력 포인터의 포인터.
 * @src_len:   남은 입력 길이의 포인터.
 * @return:    없음. 잔여(N으로 안 떨어지는 분량)는 호출자가 스칼라 처리.
 *
 * 알고리즘: VL을 svcntb()로 조회한 뒤 4개 분기 중 하나를 선택.
 *   - VL=16: 64B LUT를 4벡터에 적재, table_lookup_4vec 사용.
 *   - VL=32 또는 48: 64B LUT를 2벡터에 적재(VL이 64B 미만), table_lookup_2vec.
 *   - VL>=64: 64B LUT가 한 벡터에 들어감, svtbl_u8 직접 호출.
 * 본 루프는 svwhilelt_b8(i/3, N/3)로 활성 레인 술어를 만들고, 한 번에 vl 트리플을
 * 처리한 뒤 pred_count(true 레인 수) × 3바이트 입력, × 4바이트 출력만큼 포인터 전진.
 *
 * 호출 체인: spdk_base64_encode → [base64_encode_sve]
 * 실행 컨텍스트: 같은 스레드 동기 호출, 부수 효과 없음.
 */
static void
base64_encode_sve(char **dst, const char *enc_table, const void **src, size_t *src_len)
{
	uint64_t vl = svcntb();
	/* [한국어] 현재 SVE 구현의 벡터 길이(바이트). 16/32/48/64/.../256까지 다양. */
	svuint8_t temp0, temp1, temp2, temp3;
	/* [한국어] 6비트 4그룹 인덱스 벡터(convert_6bits_to_8bits 출력). */
	svuint8_t output0, output1, output2, output3;
	/* [한국어] LUT 룩업 후 ASCII 4문자 채널. */
	svuint8_t tbl_enc0, tbl_enc1, tbl_enc2, tbl_enc3;
	/* [한국어] 인코딩 LUT를 분할 적재할 벡터들(VL 분기에 따라 일부만 사용). */
	svuint8x4_t st_enc_output;
	/* [한국어] svst4_u8용 4채널 컴파운드(인터리브 저장). */
	svbool_t p8_all = svptrue_b8();
	/* [한국어] 모든 레인 활성 술어. LUT 적재 시 사용. */
	svbool_t pred;
	/* [한국어] 부분 벡터 처리용 동적 술어(루프마다 갱신). */
	uint64_t i = 0;
	/* [한국어] 처리한 입력 바이트 수 누적자. */
	uint64_t pred_count = 0;
	/* [한국어] 한 SIMD 반복에서 활성이었던 레인 수(처리한 트리플 수). */
	uint64_t N = (*src_len / 3) * 3;
	/* [한국어] 3바이트 단위로 정렬된 처리 대상 길이. 잔여(< 3)는 호출자가 처리. */

	if (vl == 16) {
		/* [한국어] VL=16 분기: 64B LUT를 16B씩 4벡터에 분할 적재. */

		tbl_enc0 = svld1_u8(p8_all, (uint8_t *)enc_table + 0);
		/* [한국어] LUT [0..15] 적재. */
		tbl_enc1 = svld1_u8(p8_all, (uint8_t *)enc_table + 16);
		/* [한국어] LUT [16..31] 적재. */
		tbl_enc2 = svld1_u8(p8_all, (uint8_t *)enc_table + 32);
		/* [한국어] LUT [32..47] 적재. */
		tbl_enc3 = svld1_u8(p8_all, (uint8_t *)enc_table + 48);
		/* [한국어] LUT [48..63] 적재. */

		while (i < N) {
			pred = svwhilelt_b8(i / 3, N / 3);
			/* [한국어] 술어 갱신: i/3부터 N/3까지의 트리플 인덱스 활성화.
			 * 마지막 반복에서 잔여 트리플만 활성화되어 부분 SIMD를 안전하게 처리. */

			convert_6bits_to_8bits(pred, (uint8_t *)*src, &temp0, &temp1, &temp2, &temp3);
			/* [한국어] 입력 트리플을 6비트 4그룹으로 분해. */

			table_lookup_4vec(tbl_enc0, tbl_enc1, tbl_enc2, tbl_enc3, temp0, &output0, pred, vl);
			/* [한국어] 첫 번째 6비트 그룹 → ASCII. */
			table_lookup_4vec(tbl_enc0, tbl_enc1, tbl_enc2, tbl_enc3, temp1, &output1, pred, vl);
			/* [한국어] 두 번째 6비트 그룹 → ASCII. */
			table_lookup_4vec(tbl_enc0, tbl_enc1, tbl_enc2, tbl_enc3, temp2, &output2, pred, vl);
			/* [한국어] 세 번째 6비트 그룹 → ASCII. */
			table_lookup_4vec(tbl_enc0, tbl_enc1, tbl_enc2, tbl_enc3, temp3, &output3, pred, vl);
			/* [한국어] 네 번째 6비트 그룹 → ASCII. */

			st_enc_output = svcreate4_u8(output0, output1, output2, output3);
			/* [한국어] 4개 결과를 4채널 컴파운드로 묶기(컴파일러는 인접 레지스터로 매핑). */
			svst4_u8(pred, (uint8_t *)*dst, st_enc_output);
			/* [한국어] 인터리브 저장: out0[0],out1[0],out2[0],out3[0],out0[1],... 순서. */

			pred_count = svcntp_b8(pred, pred);
			/* [한국어] 이번 반복에 처리한 활성 레인 수. */
			*src = (uint8_t *)*src + pred_count * 3;
			/* [한국어] 입력은 트리플당 3B 전진. */
			*dst += pred_count * 4;
			/* [한국어] 출력은 트리플당 4B 전진. */
			*src_len -= pred_count * 3;
			/* [한국어] 남은 입력 길이 차감. */
			i += pred_count * 3;
			/* [한국어] 처리 누적 카운터 업데이트. */

		}
	} else if (vl == 32 || vl == 48) {
		/* [한국어] VL=32 또는 48: 64B LUT를 1+1 벡터에 적재(첫 벡터는 vl B, 둘째는 64-vl B). */

		tbl_enc0 = svld1_u8(p8_all, (uint8_t *)enc_table + 0);
		/* [한국어] LUT 첫 vl 바이트 적재. */
		pred = svwhilelt_b8(vl, (uint64_t)64);
		/* [한국어] 잔여(64-vl) 바이트만 활성화하는 술어. */
		tbl_enc1 = svld1_u8(pred, (uint8_t *)enc_table + vl);
		/* [한국어] LUT 둘째 청크 적재(부분 술어로 잔여만 적재). */

		while (i < N) {
			pred = svwhilelt_b8(i / 3, N / 3);
			/* [한국어] 처리할 트리플 범위 술어 갱신. */

			convert_6bits_to_8bits(pred, (uint8_t *)*src, &temp0, &temp1, &temp2, &temp3);
			/* [한국어] 입력을 6비트 4그룹으로 분해. */

			table_lookup_2vec(tbl_enc0, tbl_enc1, temp0, &output0, pred, vl);
			/* [한국어] 첫 6비트 그룹 룩업. */
			table_lookup_2vec(tbl_enc0, tbl_enc1, temp1, &output1, pred, vl);
			/* [한국어] 두 번째. */
			table_lookup_2vec(tbl_enc0, tbl_enc1, temp2, &output2, pred, vl);
			/* [한국어] 세 번째. */
			table_lookup_2vec(tbl_enc0, tbl_enc1, temp3, &output3, pred, vl);
			/* [한국어] 네 번째. */

			st_enc_output = svcreate4_u8(output0, output1, output2, output3);
			/* [한국어] 4채널 묶기. */
			svst4_u8(pred, (uint8_t *)*dst, st_enc_output);
			/* [한국어] 인터리브 저장. */

			pred_count = svcntp_b8(pred, pred);
			/* [한국어] 활성 레인 수. */
			*src = (uint8_t *)*src + pred_count * 3;
			/* [한국어] 입력 전진. */
			*dst += pred_count * 4;
			/* [한국어] 출력 전진. */
			*src_len -= pred_count * 3;
			/* [한국어] 남은 길이 차감. */
			i += pred_count * 3;
			/* [한국어] 처리 누적자 갱신. */

		}
	} else if (vl >= 64) {
		/* [한국어] VL >= 64: 64B LUT가 한 SVE 벡터에 들어감 → 단일 svtbl_u8로 룩업 가능. */

		pred = svwhilelt_b8((uint64_t)0, (uint64_t)64);
		/* [한국어] LUT 적재용 술어: 첫 64레인만 활성. */
		tbl_enc0 = svld1_u8(pred, (uint8_t *)enc_table);
		/* [한국어] 64B LUT를 한 번에 적재. */

		while (i < N) {
			pred = svwhilelt_b8(i / 3, N / 3);
			/* [한국어] 처리할 트리플 범위 술어. */

			convert_6bits_to_8bits(pred, (uint8_t *)*src, &temp0, &temp1, &temp2, &temp3);
			/* [한국어] 6비트 4그룹 분해. */

			output0 = svtbl_u8(tbl_enc0, temp0);
			/* [한국어] 직접 svtbl_u8 룩업: 인덱스 0..63 모두 같은 한 벡터로 매핑. */
			output1 = svtbl_u8(tbl_enc0, temp1);
			/* [한국어] 동일 처리. */
			output2 = svtbl_u8(tbl_enc0, temp2);
			/* [한국어] 동일 처리. */
			output3 = svtbl_u8(tbl_enc0, temp3);
			/* [한국어] 동일 처리. */

			st_enc_output = svcreate4_u8(output0, output1, output2, output3);
			/* [한국어] 4채널 묶기. */
			svst4_u8(pred, (uint8_t *)*dst, st_enc_output);
			/* [한국어] 인터리브 저장. */

			pred_count = svcntp_b8(pred, pred);
			/* [한국어] 활성 레인 수. */
			*src = (uint8_t *)*src + pred_count * 3;
			/* [한국어] 입력 전진. */
			*dst += pred_count * 4;
			/* [한국어] 출력 전진. */
			*src_len -= pred_count * 3;
			/* [한국어] 남은 길이 차감. */
			i += pred_count * 3;
			/* [한국어] 처리 누적자 갱신. */

		}
	}
}

/*
 * [한국어]
 * base64_decode_sve - SVE를 사용해 base64 ASCII → 원본 바이트로 디코드.
 *
 * @dst:       출력 포인터의 포인터(전진).
 * @dec_table: 128엔트리 디코드 LUT(0..127 ASCII → 6비트 또는 0xFF invalid).
 * @src:       입력 ASCII 포인터의 포인터.
 * @src_len:   남은 입력 길이의 포인터.
 * @return:    없음. invalid 검출 시 즉시 return으로 SIMD 탈출, 호출자가 잔여 처리.
 *
 * 알고리즘: VL에 따라 LUT 적재 전략을 분기 (16/32/48/64-112/128+).
 * 단계: svld4_u8로 4채널 deinterleave → 모든 채널이 ASCII 7비트(<128) 검사 →
 * LUT 룩업(table_lookup_*vec 또는 svtbl_u8) → invalid 검사 → 6→8 패킹 → svst3_u8 저장.
 *
 * 호출 체인: spdk_base64_decode → [base64_decode_sve]
 * 실행 컨텍스트: 같은 스레드, 부수 효과 없음.
 */
static void
base64_decode_sve(void **dst, const uint8_t *dec_table, const uint8_t **src, size_t *src_len)
{
	uint64_t vl = svcntb();
	/* [한국어] 현재 SVE 벡터 길이(바이트). */
	svuint8_t str0, str1, str2, str3;
	/* [한국어] 입력 4채널 deinterleave 결과. */
	svuint8_t temp0, temp1, temp2, temp3;
	/* [한국어] LUT 룩업 후 6비트값(또는 0xFF) 4채널. */
	svuint8_t output0, output1, output2;
	/* [한국어] 디코드 결과 8비트 3채널. */
	svuint8_t tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, tbl_dec4, tbl_dec5, tbl_dec6, tbl_dec7;
	/* [한국어] 128B 디코드 LUT를 분할 적재할 8개 벡터(VL 분기에 따라 일부만 사용). */
	svuint8x3_t st_dec_output;
	/* [한국어] svst3_u8용 3채널 컴파운드. */
	svbool_t p8_all = svptrue_b8();
	/* [한국어] 모든 레인 활성 술어. */
	svbool_t pred;
	/* [한국어] 동적 술어. */
	uint64_t i = 0;
	/* [한국어] 처리 진행 누적 카운터(바이트 기준). */
	uint64_t pred_count = 0;
	/* [한국어] 한 반복 활성 레인 수. */
	uint64_t N = (*src_len / 4) * 4;
	/* [한국어] 4바이트(=한 base64 그룹) 단위로 정렬된 처리 길이. */
	svuint8x4_t ld_dec_input;
	/* [한국어] svld4_u8 결과를 받을 4채널 컴파운드. */

	if (vl == 16) {
		/* [한국어] VL=16 분기: 128B LUT를 16B씩 8벡터로 적재 → table_lookup_8vec 사용. */
		tbl_dec0 = svld1_u8(p8_all, (uint8_t *)dec_table + 0);
		/* [한국어] LUT [0..15] 적재. (실제로는 [0..31] 영역이 모두 invalid 0xFF이므로 사용 안 됨). */
		tbl_dec1 = svld1_u8(p8_all, (uint8_t *)dec_table + 16);
		/* [한국어] LUT [16..31]. (실제 사용 안 함, table_lookup_8vec가 0..31 청크 스킵). */
		tbl_dec2 = svld1_u8(p8_all, (uint8_t *)dec_table + 32);
		/* [한국어] LUT [32..47] - '+', '/', '0'-'9' 등이 포함. */
		tbl_dec3 = svld1_u8(p8_all, (uint8_t *)dec_table + 48);
		/* [한국어] LUT [48..63] - 추가 0..9 부분과 invalid. */
		tbl_dec4 = svld1_u8(p8_all, (uint8_t *)dec_table + 64);
		/* [한국어] LUT [64..79] - '@', 'A'-'O'. */
		tbl_dec5 = svld1_u8(p8_all, (uint8_t *)dec_table + 80);
		/* [한국어] LUT [80..95] - 'P'-'_'. */
		tbl_dec6 = svld1_u8(p8_all, (uint8_t *)dec_table + 96);
		/* [한국어] LUT [96..111] - '`', 'a'-'o'. */
		tbl_dec7 = svld1_u8(p8_all, (uint8_t *)dec_table + 112);
		/* [한국어] LUT [112..127] - 'p'-'~'. */

		while (i < N) {
			pred = svwhilelt_b8(i / 4, N / 4);
			/* [한국어] 처리할 base64 그룹(4B 단위) 범위 술어. */

			ld_dec_input = svld4_u8(pred, *src);
			/* [한국어] 4채널 deinterleave 로드: c0/c1/c2/c3가 각각 같은 그룹의 1/2/3/4번 문자. */

			str0 = svget4_u8(ld_dec_input, 0);
			/* [한국어] 채널 0 추출(각 그룹의 첫 ASCII 문자). */
			str1 = svget4_u8(ld_dec_input, 1);
			/* [한국어] 채널 1(두 번째 문자). */
			str2 = svget4_u8(ld_dec_input, 2);
			/* [한국어] 채널 2(세 번째 문자). */
			str3 = svget4_u8(ld_dec_input, 3);
			/* [한국어] 채널 3(네 번째 문자). */

			if (svcntp_b8(pred, svcmpge_n_u8(pred, str0, 128))) { return; }
			/* [한국어] 채널 0에 ASCII 범위 외(>= 128) 바이트가 있으면 즉시 종료. 호출자가 처리. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str1, 128))) { return; }
			/* [한국어] 채널 1 검사. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str2, 128))) { return; }
			/* [한국어] 채널 2 검사. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str3, 128))) { return; }
			/* [한국어] 채널 3 검사. */

			if (table_lookup_8vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, tbl_dec4, tbl_dec5, tbl_dec6,
					      tbl_dec7, str0, &temp0, pred, vl)) { return; }
			/* [한국어] 채널 0에 대해 LUT 룩업, invalid면 즉시 종료. */
			if (table_lookup_8vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, tbl_dec4, tbl_dec5, tbl_dec6,
					      tbl_dec7, str1, &temp1, pred, vl)) { return; }
			/* [한국어] 채널 1 룩업. */
			if (table_lookup_8vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, tbl_dec4, tbl_dec5, tbl_dec6,
					      tbl_dec7, str2, &temp2, pred, vl)) { return; }
			/* [한국어] 채널 2 룩업. */
			if (table_lookup_8vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, tbl_dec4, tbl_dec5, tbl_dec6,
					      tbl_dec7, str3, &temp3, pred, vl)) { return; }
			/* [한국어] 채널 3 룩업. */

			convert_8bits_to_6bits(pred, temp0, temp1, temp2, temp3, &output0, &output1, &output2);
			/* [한국어] 4×6비트 → 3×8비트 패킹. */

			st_dec_output = svcreate3_u8(output0, output1, output2);
			/* [한국어] 3채널 컴파운드 묶기. */
			svst3_u8(pred, (uint8_t *)*dst, st_dec_output);
			/* [한국어] 인터리브 저장: 자연스러운 바이트 순서로 출력. */

			pred_count = svcntp_b8(pred, pred);
			/* [한국어] 활성 레인(=처리한 4B 그룹) 수. */
			*src += pred_count * 4;
			/* [한국어] 입력은 그룹당 4B 전진. */
			*dst = (uint8_t *)*dst + pred_count * 3;
			/* [한국어] 출력은 그룹당 3B 전진. */
			*src_len -= pred_count * 4;
			/* [한국어] 남은 입력 길이 차감. */
			i += pred_count * 4;
			/* [한국어] 처리 누적자 갱신. */

		}
	} else if (vl == 32) {
		/* [한국어] VL=32 분기: 128B LUT를 32B 4벡터로 분할 → table_lookup_4vec. */
		tbl_dec0 = svld1_u8(p8_all, (uint8_t *)dec_table + 0);
		/* [한국어] LUT [0..31] (vl=32). */
		tbl_dec1 = svld1_u8(p8_all, (uint8_t *)dec_table + vl);
		/* [한국어] LUT [32..63]. */
		tbl_dec2 = svld1_u8(p8_all, (uint8_t *)dec_table + vl * 2);
		/* [한국어] LUT [64..95]. */
		tbl_dec3 = svld1_u8(p8_all, (uint8_t *)dec_table + vl * 3);
		/* [한국어] LUT [96..127]. */

		while (i < N) {
			pred = svwhilelt_b8(i / 4, N / 4);
			/* [한국어] 처리 범위 술어. i/4..N/4의 base64 그룹 인덱스 활성화. */

			ld_dec_input = svld4_u8(pred, *src);
			/* [한국어] 4채널 deinterleave 로드: 각 그룹의 c0/c1/c2/c3 ASCII 문자. */

			str0 = svget4_u8(ld_dec_input, 0);
			/* [한국어] 채널 0(첫 ASCII 문자) 추출. */
			str1 = svget4_u8(ld_dec_input, 1);
			/* [한국어] 채널 1(두 번째). */
			str2 = svget4_u8(ld_dec_input, 2);
			/* [한국어] 채널 2(세 번째). */
			str3 = svget4_u8(ld_dec_input, 3);
			/* [한국어] 채널 3(네 번째). */

			if (svcntp_b8(pred, svcmpge_n_u8(pred, str0, 128))) { return; }
			/* [한국어] 채널 0에 ASCII 범위 외(>=128) 바이트가 활성 레인 중 하나라도 있으면
			 * 즉시 함수 반환 → 호출자가 잔여를 스칼라로 처리. svcntp_b8은 활성 레인 카운트. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str1, 128))) { return; }
			/* [한국어] 채널 1 검사. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str2, 128))) { return; }
			/* [한국어] 채널 2 검사. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str3, 128))) { return; }
			/* [한국어] 채널 3 검사. */

			if (table_lookup_4vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, str0, &temp0, pred, vl)) { return; }
			/* [한국어] 채널 0 LUT 룩업, invalid시 즉시 종료. */
			if (table_lookup_4vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, str1, &temp1, pred, vl)) { return; }
			/* [한국어] 채널 1 LUT 룩업. */
			if (table_lookup_4vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, str2, &temp2, pred, vl)) { return; }
			/* [한국어] 채널 2 LUT 룩업. */
			if (table_lookup_4vec(tbl_dec0, tbl_dec1, tbl_dec2, tbl_dec3, str3, &temp3, pred, vl)) { return; }
			/* [한국어] 채널 3 LUT 룩업. */

			convert_8bits_to_6bits(pred, temp0, temp1, temp2, temp3, &output0, &output1, &output2);
			/* [한국어] 4×6비트 → 3×8비트 패킹. */

			st_dec_output = svcreate3_u8(output0, output1, output2);
			/* [한국어] 3채널 컴파운드 묶기. */
			svst3_u8(pred, (uint8_t *)*dst, st_dec_output);
			/* [한국어] 인터리브 저장(자연스러운 바이트 순서). */

			pred_count = svcntp_b8(pred, pred);
			/* [한국어] 이번 반복에 처리한 활성 레인 수. */
			*src += pred_count * 4;
			/* [한국어] 입력은 그룹당 4B 전진. */
			*dst = (uint8_t *)*dst + pred_count * 3;
			/* [한국어] 출력은 그룹당 3B 전진. */
			*src_len -= pred_count * 4;
			/* [한국어] 남은 입력 길이 차감. */
			i += pred_count * 4;
			/* [한국어] 처리 누적자 갱신. */

		}

	} else if (vl == 48) {
		/* [한국어] VL=48 분기: 128B LUT를 48B 3벡터로 분할(마지막은 부분 적재) → table_lookup_3vec. */
		tbl_dec0 = svld1_u8(p8_all, (uint8_t *)dec_table + 0);
		/* [한국어] LUT [0..47]. */
		tbl_dec1 = svld1_u8(p8_all, (uint8_t *)dec_table + vl);
		/* [한국어] LUT [48..95]. */
		pred = svwhilelt_b8(vl * 2, (uint64_t)128);
		/* [한국어] 마지막 청크 크기(=128 - 96 = 32B)만큼만 활성 술어. */
		tbl_dec2 = svld1_u8(pred, (uint8_t *)dec_table + 2 * vl);
		/* [한국어] LUT [96..127]을 부분 술어로 적재(나머지 레인은 0). */

		while (i < N) {
			pred = svwhilelt_b8(i / 4, N / 4);
			/* [한국어] 처리 범위 술어. i/4..N/4의 base64 그룹 인덱스만 활성. */

			ld_dec_input = svld4_u8(pred, *src);
			/* [한국어] 4채널 deinterleave 로드(c0/c1/c2/c3). */

			str0 = svget4_u8(ld_dec_input, 0);
			/* [한국어] 채널 0 추출. */
			str1 = svget4_u8(ld_dec_input, 1);
			/* [한국어] 채널 1 추출. */
			str2 = svget4_u8(ld_dec_input, 2);
			/* [한국어] 채널 2 추출. */
			str3 = svget4_u8(ld_dec_input, 3);
			/* [한국어] 채널 3 추출. */

			if (svcntp_b8(pred, svcmpge_n_u8(pred, str0, 128))) { return; }
			/* [한국어] 채널 0 ASCII 범위 외(>=128) 검사 → 발견 시 함수 종료(스칼라 폴백). */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str1, 128))) { return; }
			/* [한국어] 채널 1. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str2, 128))) { return; }
			/* [한국어] 채널 2. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str3, 128))) { return; }
			/* [한국어] 채널 3. */

			if (table_lookup_3vec(tbl_dec0, tbl_dec1, tbl_dec2, str0, &temp0, pred, vl)) { return; }
			/* [한국어] 채널 0 LUT 룩업(3개 청크 분할). invalid 시 종료. */
			if (table_lookup_3vec(tbl_dec0, tbl_dec1, tbl_dec2, str1, &temp1, pred, vl)) { return; }
			/* [한국어] 채널 1. */
			if (table_lookup_3vec(tbl_dec0, tbl_dec1, tbl_dec2, str2, &temp2, pred, vl)) { return; }
			/* [한국어] 채널 2. */
			if (table_lookup_3vec(tbl_dec0, tbl_dec1, tbl_dec2, str3, &temp3, pred, vl)) { return; }
			/* [한국어] 채널 3. */

			convert_8bits_to_6bits(pred, temp0, temp1, temp2, temp3, &output0, &output1, &output2);
			/* [한국어] 4×6비트 → 3×8비트 패킹. */

			st_dec_output = svcreate3_u8(output0, output1, output2);
			/* [한국어] 3채널 묶기. */
			svst3_u8(pred, (uint8_t *)*dst, st_dec_output);
			/* [한국어] 인터리브 저장. */

			pred_count = svcntp_b8(pred, pred);
			/* [한국어] 활성 레인 수. */
			*src += pred_count * 4;
			/* [한국어] 입력 그룹당 4B 전진. */
			*dst = (uint8_t *)*dst + pred_count * 3;
			/* [한국어] 출력 그룹당 3B 전진. */
			*src_len -= pred_count * 4;
			/* [한국어] 남은 입력 길이 차감. */
			i += pred_count * 4;
			/* [한국어] 처리 누적자 갱신. */

		}
	} else if (vl == 64 || vl == 80 || vl == 96 || vl == 112) {
		/* [한국어] VL=64..112 분기: 128B LUT를 2개 벡터(완전+부분)로 적재 → table_lookup_2vec. */
		tbl_dec0 = svld1_u8(p8_all, (uint8_t *)dec_table + 0);
		/* [한국어] LUT [0..vl-1] 적재. */
		pred = svwhilelt_b8(vl, (uint64_t)128);
		/* [한국어] 잔여(=128-vl) 만큼 활성 술어. */
		tbl_dec1 = svld1_u8(pred, (uint8_t *)dec_table + vl);
		/* [한국어] LUT 둘째 청크를 부분 술어로 적재. */

		while (i < N) {
			pred = svwhilelt_b8(i / 4, N / 4);
			/* [한국어] 처리 범위 술어. */

			ld_dec_input = svld4_u8(pred, *src);
			/* [한국어] 4채널 deinterleave 로드. */

			str0 = svget4_u8(ld_dec_input, 0);
			/* [한국어] 채널 0 추출. */
			str1 = svget4_u8(ld_dec_input, 1);
			/* [한국어] 채널 1 추출. */
			str2 = svget4_u8(ld_dec_input, 2);
			/* [한국어] 채널 2 추출. */
			str3 = svget4_u8(ld_dec_input, 3);
			/* [한국어] 채널 3 추출. */

			if (svcntp_b8(pred, svcmpge_n_u8(pred, str0, 128))) { return; }
			/* [한국어] 채널 0 ASCII 범위 외 검사 → 발견 시 즉시 종료. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str1, 128))) { return; }
			/* [한국어] 채널 1. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str2, 128))) { return; }
			/* [한국어] 채널 2. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str3, 128))) { return; }
			/* [한국어] 채널 3. */

			if (table_lookup_2vec(tbl_dec0, tbl_dec1, str0, &temp0, pred, vl)) { return; }
			/* [한국어] 채널 0 LUT 룩업(2개 청크 분할). invalid 시 종료. */
			if (table_lookup_2vec(tbl_dec0, tbl_dec1, str1, &temp1, pred, vl)) { return; }
			/* [한국어] 채널 1. */
			if (table_lookup_2vec(tbl_dec0, tbl_dec1, str2, &temp2, pred, vl)) { return; }
			/* [한국어] 채널 2. */
			if (table_lookup_2vec(tbl_dec0, tbl_dec1, str3, &temp3, pred, vl)) { return; }
			/* [한국어] 채널 3. */

			convert_8bits_to_6bits(pred, temp0, temp1, temp2, temp3, &output0, &output1, &output2);
			/* [한국어] 4×6비트 → 3×8비트 패킹. */

			st_dec_output = svcreate3_u8(output0, output1, output2);
			/* [한국어] 3채널 묶기. */
			svst3_u8(pred, (uint8_t *)*dst, st_dec_output);
			/* [한국어] 인터리브 저장. */

			pred_count = svcntp_b8(pred, pred);
			/* [한국어] 활성 레인 수. */
			*src += pred_count * 4;
			/* [한국어] 입력 그룹당 4B 전진. */
			*dst = (uint8_t *)*dst + pred_count * 3;
			/* [한국어] 출력 그룹당 3B 전진. */
			*src_len -= pred_count * 4;
			/* [한국어] 남은 입력 길이 차감. */
			i += pred_count * 4;
			/* [한국어] 처리 누적자 갱신. */

		}
	} else if (vl >= 128) {
		/* [한국어] VL>=128 분기: 128B LUT 전체가 한 SVE 벡터에 들어감 → svtbl_u8 직접 사용. */
		pred = svwhilelt_b8((uint64_t)0, (uint64_t)128);
		/* [한국어] 첫 128레인 활성 술어. */
		tbl_dec0 = svld1_u8(pred, (uint8_t *)dec_table + 0);
		/* [한국어] 128B LUT를 한 벡터에 적재(VL의 일부분만 사용). */

		while (i < N) {
			pred = svwhilelt_b8(i / 4, N / 4);
			/* [한국어] 처리 범위 술어. */

			ld_dec_input = svld4_u8(pred, *src);
			/* [한국어] 4채널 deinterleave 로드. */

			str0 = svget4_u8(ld_dec_input, 0);
			/* [한국어] 채널 0 추출. */
			str1 = svget4_u8(ld_dec_input, 1);
			/* [한국어] 채널 1 추출. */
			str2 = svget4_u8(ld_dec_input, 2);
			/* [한국어] 채널 2 추출. */
			str3 = svget4_u8(ld_dec_input, 3);
			/* [한국어] 채널 3 추출. */

			if (svcntp_b8(pred, svcmpge_n_u8(pred, str0, 128))) { return; }
			/* [한국어] 채널 0 ASCII 범위 외 검사. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str1, 128))) { return; }
			/* [한국어] 채널 1. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str2, 128))) { return; }
			/* [한국어] 채널 2. */
			if (svcntp_b8(pred, svcmpge_n_u8(pred, str3, 128))) { return; }
			/* [한국어] 채널 3. */

			temp0 = svtbl_u8(tbl_dec0, str0);
			/* [한국어] 단일 svtbl_u8 룩업: 인덱스 0..127 모두 한 벡터에서 룩업 가능
			 * (VL>=128이므로 LUT가 한 SVE 벡터에 통째로 들어감). */
			temp1 = svtbl_u8(tbl_dec0, str1);
			/* [한국어] 채널 1 단일 룩업. */
			temp2 = svtbl_u8(tbl_dec0, str2);
			/* [한국어] 채널 2 단일 룩업. */
			temp3 = svtbl_u8(tbl_dec0, str3);
			/* [한국어] 채널 3 단일 룩업. */

			if (svcntp_b8(pred, svcmpeq_n_u8(pred, temp0, 255))) { return; }
			/* [한국어] 채널 0 LUT 룩업 결과에 0xFF(invalid)가 있으면 즉시 종료. */
			if (svcntp_b8(pred, svcmpeq_n_u8(pred, temp1, 255))) { return; }
			/* [한국어] 채널 1 invalid 검사. */
			if (svcntp_b8(pred, svcmpeq_n_u8(pred, temp2, 255))) { return; }
			/* [한국어] 채널 2 invalid 검사. */
			if (svcntp_b8(pred, svcmpeq_n_u8(pred, temp3, 255))) { return; }
			/* [한국어] 채널 3 invalid 검사. */

			convert_8bits_to_6bits(pred, temp0, temp1, temp2, temp3, &output0, &output1, &output2);
			/* [한국어] 4×6비트 → 3×8비트 패킹. */

			st_dec_output = svcreate3_u8(output0, output1, output2);
			/* [한국어] 3채널 묶기. */
			svst3_u8(pred, (uint8_t *)*dst, st_dec_output);
			/* [한국어] 인터리브 저장. */

			pred_count = svcntp_b8(pred, pred);
			/* [한국어] 활성 레인 수. */
			*src += pred_count * 4;
			/* [한국어] 입력 그룹당 4B 전진. */
			*dst = (uint8_t *)*dst + pred_count * 3;
			/* [한국어] 출력 그룹당 3B 전진. */
			*src_len -= pred_count * 4;
			/* [한국어] 남은 입력 길이 차감. */
			i += pred_count * 4;
			/* [한국어] 처리 누적자 갱신. */

		}
	}
}
