/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

/** \file
 * General utility functions
 */

/*
 * [한국어 설명] SPDK 공통 유틸리티 매크로·함수 (util.h)
 *
 * === 파일의 역할 ===
 * SPDK 전반에서 반복적으로 필요한 저수준 유틸리티를 한 곳에 모아 제공한다.
 * 제공 범주:
 *   (1) min/max, COUNTOF, CONTAINEROF, SIZEOF, SIZEOF_MEMBER, COUNTOF_MEMBER
 *   (2) 시간 단위 상수 (초→ms/us/ns)
 *   (3) 올림 나눗셈, 2의 거듭제곱 정렬(FLOOR/CEIL), 비트 매크로 SPDK_BIT
 *   (4) 전방 호환 구조체 필드 검증: SPDK_FIELD_VALID / SPDK_GET_FIELD
 *   (5) log2, 2의 거듭제곱 반올림 (align32/64pow2), pow2 여부 체크
 *   (6) round_up / divide_round_up (호스트 64비트 버전)
 *   (7) iovec 순회자(iov iterator) API: spdk_ioviter · spdk_iov_xfer
 *   (8) iovec 복사/메모리셋/단일 iovec 구성 헬퍼
 *   (9) 32-bit serial number 산술 (RFC 1982 근사) — sn32_add/lt/gt
 *   (10) 안전한 memset (spdk_memset_s) — 컴파일러가 dead store 제거 방지
 *   (11) scan-build 오탐 무력화용 더미 포인터 배열 초기화 매크로
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK의 가장 하단 유틸 계층. 공개 헤더 상당수(bdev.h, nvme.h 등)가 이
 * 헤더의 매크로에 의존한다. 구현은 lib/util/*.
 * 실행 컨텍스트: 모든 컨텍스트에서 사용 가능. 매크로는 런타임 비용 0,
 * static inline 함수는 수 사이클, iov 관련 함수는 메모리 복사 기반.
 *
 * === 타 모듈과의 연결 ===
 * 의존:
 *   - spdk/stdinc.h - 표준 타입
 *   - spdk/fd.h     - 파일 유틸 (일부 유틸이 함께 묶임)
 * 의존하는 모듈: 거의 모든 SPDK 코드
 * 공유 자료구조:
 *   - struct spdk_single_ioviter: 단일 iovec 배열 순회 상태
 *   - struct spdk_ioviter:        N개 iovec 배열의 병행 순회 상태
 *   - struct spdk_iov_xfer:       iovec에 대한 "진행 포인터" (복사 재개용)
 *
 * === 주요 함수/구조체 요약 ===
 *   - SPDK_CACHE_LINE_SIZE, spdk_min/max, SPDK_COUNTOF, SPDK_CONTAINEROF
 *   - SPDK_ALIGN_FLOOR/CEIL, SPDK_BIT
 *   - SPDK_FIELD_VALID/GET_FIELD: ABI 진화 시 선택적 필드 접근
 *   - spdk_u32log2/u64log2/align32pow2/align64pow2/u32_is_pow2/u64_is_pow2
 *   - spdk_divide_round_up/round_up
 *   - spdk_ioviter_*: N-way iovec 교차 순회
 *   - spdk_iov_xfer_*: iovec 부분 채우기/빼내기
 *   - spdk_iovcpy/iovmove: iovec 간 복사
 *   - spdk_iov_memset, SPDK_IOV_ONE
 *   - spdk_sn32_*: 32비트 시퀀스 번호 비교
 *   - spdk_memset_s: 최적화 제거 방지 memset
 *   - spdk_rand_xorshift64(_seed): 빠른 의사난수
 */

#ifndef SPDK_UTIL_H              /* [한국어] include 가드 */
#define SPDK_UTIL_H

/* memset_s is only available if __STDC_WANT_LIB_EXT1__ is set to 1 before including \<string.h\> */
#define __STDC_WANT_LIB_EXT1__ 1
                                 /* [한국어] C11 Annex K 보안 함수(memset_s 등)를 노출하도록 표준 헤더 이전에 정의
                                  *  - stdinc.h가 <string.h>를 포함하기 전에 이 매크로가 보여야 memset_s 심볼이 선언됨 */

#include "spdk/stdinc.h"         /* [한국어] 표준 타입과 string.h 등 확보 */
#include "spdk/fd.h"             /* [한국어] 편의를 위해 fd 유틸도 함께 포함 — 역사적 관례 */

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_CACHE_LINE_SIZE 64
                                 /* [한국어] 현대 x86/ARM의 캐시 라인 크기(바이트)
                                  *  - 구조체 멤버 정렬, false-sharing 회피용 패딩에 사용
                                  *  - 일부 POWER 등에서는 128바이트지만 SPDK는 64로 고정 (과다 정렬 허용 가정) */

#define spdk_min(a,b) (((a)<(b))?(a):(b))
                                 /* [한국어] 최솟값 매크로 — 표준 min이 없어 재정의
                                  *  - 부작용 주의: a, b가 두 번 평가됨. 복잡한 식은 호출 전에 변수로 대입 */
#define spdk_max(a,b) (((a)>(b))?(a):(b))
                                 /* [한국어] 최댓값 매크로 — 동일한 이중 평가 주의 */

#define SPDK_COUNTOF(arr) (sizeof(arr) / sizeof((arr)[0]))
                                 /* [한국어] 정적 배열의 원소 수
                                  *  - 포인터를 넘기면 잘못된 값 반환(컴파일러 경고) — 배열 타입 전용 */

#define SPDK_CONTAINEROF(ptr, type, member) ((type *)((uintptr_t)ptr - offsetof(type, member)))
                                 /* [한국어] 멤버 포인터에서 컨테이너 구조체 포인터 복원 (리눅스 container_of 관용구)
                                  *  - SPDK intrusive 리스트/트리에서 list entry → user struct 변환의 핵심 */

/** Returns size of an object pointer by ptr up to and including member */
#define SPDK_SIZEOF(ptr, member) (offsetof(__typeof__(*(ptr)), member) + sizeof((ptr)->member))
                                 /* [한국어] ptr이 가리키는 타입의 "member까지 포함한" 최소 크기
                                  *  - 전방 호환 구조체에서 특정 필드까지만 복사할 때 사용
                                  *  - __typeof__는 GCC 확장 (C++에서는 decltype 대응) */

/**
 * Get the size of a member of a struct.
 */
#define SPDK_SIZEOF_MEMBER(type, member) (sizeof(((type *)0)->member))
                                 /* [한국어] 구조체 멤버 필드의 sizeof — NULL 포인터 역참조는 sizeof 안에서 미실행되므로 안전 */

/**
 * Get the number of elements in an array of a struct member
 */
#define SPDK_COUNTOF_MEMBER(type, member) (SPDK_COUNTOF(((type *)0)->member))
                                 /* [한국어] 구조체 멤버 배열의 원소 수 */

#define SPDK_SEC_TO_MSEC 1000ULL
#define SPDK_SEC_TO_USEC 1000000ULL
#define SPDK_SEC_TO_NSEC 1000000000ULL
                                 /* [한국어] 시간 단위 변환 상수 — 64비트 리터럴로 오버플로 방지 */

#define SPDK_MSEC_TO_USEC 1000ULL
                                 /* [한국어] 밀리초 → 마이크로초 계수 */

/* Ceiling division of unsigned integers */
#define SPDK_CEIL_DIV(x,y) (((x)+(y)-1)/(y))
                                 /* [한국어] 올림 나눗셈: ceil(x/y). 둘 다 unsigned여야 정확.
                                  *  - 주의: (x + y - 1)에서 오버플로 가능 — 매우 큰 값은 호출 전 체크 */

/**
 * Macro to align a value to a given power-of-two. The resultant value
 * will be of the same type as the first parameter, and will be no
 * bigger than the first parameter. Second parameter must be a
 * power-of-two value.
 */
#define SPDK_ALIGN_FLOOR(val, align) \
	(__typeof__(val))((val) & (~((__typeof__(val))((align) - 1))))
                                 /* [한국어] val을 align의 배수로 내림. align은 반드시 2의 거듭제곱
                                  *  - `align - 1`이 하위 비트 마스크. ~로 상위 비트만 남김 */
/**
 * Macro to align a value to a given power-of-two. The resultant value
 * will be of the same type as the first parameter, and will be no lower
 * than the first parameter. Second parameter must be a power-of-two
 * value.
 */
#define SPDK_ALIGN_CEIL(val, align) \
	SPDK_ALIGN_FLOOR(((val) + ((__typeof__(val)) (align) - 1)), align)
                                 /* [한국어] val을 align의 배수로 올림. (val+align-1)에 FLOOR 적용하는 고전 관용구 */

#define SPDK_BIT(n) (1ul << (n))
                                 /* [한국어] n번 비트만 1로 설정된 값. unsigned long 리터럴 (64bit 플랫폼에서는 64bit) */

/**
 * Check if a given field is valid in a structure with size tracking. The third
 * parameter is optional and can be used to specify the size of the object.  If
 * unset, (obj)->size will be used by default.
 */
#define SPDK_FIELD_VALID(obj, field, ...) \
	_SPDK_FIELD_VALID(obj, field, ## __VA_ARGS__, (obj)->size)
                                 /* [한국어] ABI 진화 시 필드 존재 여부를 런타임에 판정
                                  *  - SPDK의 확장 가능 구조체는 첫 필드로 size를 두어 호출자가 인지한 버전을 명시
                                  *  - 가변 인자 트릭: 세 번째 인자가 없으면 (obj)->size 사용, 있으면 그 값 사용 */

#define _SPDK_FIELD_VALID(obj, field, size, ...) \
	((size) >= (offsetof(__typeof__(*(obj)), field) + sizeof((obj)->field)))
                                 /* [한국어] 내부 구현: 구조체 선언된 크기 >= 필드 offset + 필드 크기 여야 해당 필드가 실제 존재 */
/**
 * Get a field from a structure with size tracking.  The fourth parameter is
 * optional and can be used to specify the size of the object.  If unset,
 * (obj)->size will be used by default.
 */
#define SPDK_GET_FIELD(obj, field, defval, ...) \
	_SPDK_GET_FIELD(obj, field, defval, ## __VA_ARGS__, (obj)->size)
                                 /* [한국어] 필드가 유효하면 그 값을, 아니면 defval을 반환
                                  *  - 구버전 호출자의 구조체에서는 신규 필드가 없어도 안전하게 기본값으로 후퇴 */

#define _SPDK_GET_FIELD(obj, field, defval, size, ...) \
	(SPDK_FIELD_VALID(obj, field, size) ? (obj)->field : (defval))
                                 /* [한국어] 내부: FIELD_VALID 결과에 따라 분기 */

uint32_t spdk_u32log2(uint32_t x);
/*
 * [한국어]
 * spdk_u32log2 - floor(log2(x)) 반환 (x >= 1 전제, 0 입력은 구현에 의존)
 *
 * @x: 비영 32비트 값
 * @return: 가장 높은 set bit의 인덱스 (0 기반)
 *
 * 구현은 lib/util/math.c. 빌트인 __builtin_clz 등을 활용해 O(1).
 * 사용처: 큐 사이즈 pow2 계산, bit shift 기반 인덱스 매핑.
 */

/**
 * Generate a 64-bit pseudo-random number using xorshift algorithm.
 *
 * \param state the current seed value.
 * \return a new pseudo-random 64-bit number.
 */
static inline uint64_t
spdk_rand_xorshift64(uint64_t *state)
/*
 * [한국어]
 * spdk_rand_xorshift64 - xorshift64 PRNG
 *
 * 품질 중간, 속도 최상(3개 shift+xor만). 암호학적 용도 금지.
 * 호출 시 state가 업데이트되어 다음 호출에 영향 — 호출자 소유.
 */
{
	uint64_t x = *state;         /* [한국어] 현재 시드 로드 */

	x ^= x << 13;                /* [한국어] 왼쪽 13비트 shift 후 XOR — 비트 확산 */
	x ^= x >> 7;                 /* [한국어] 오른쪽 7비트 shift XOR */
	x ^= x << 17;                /* [한국어] 왼쪽 17비트 shift XOR (Marsaglia 고전 파라미터) */

	*state = x;                  /* [한국어] 업데이트된 상태 저장 */
	return x;                    /* [한국어] 생성된 난수 반환 */
}

/**
 * Generate a non-zero initial seed for xorshift64.
 *
 * \return a random 64-bit seed value(non-zero).
 */
static inline uint64_t
spdk_rand_xorshift64_seed(void)
/*
 * [한국어]
 * spdk_rand_xorshift64_seed - xorshift64 초기 시드 생성
 *
 * libc rand()를 두 번 조합해 64비트 시드 구성. rand()는 암호학 안전 아님 —
 * 고품질 필요 시 호출자가 getrandom 등으로 교체.
 * xorshift64는 0 시드에서 0만 생성하므로 zero 방지 처리.
 */
{
	uint64_t seed = ((uint64_t)rand() << 32) | rand();
                                 /* [한국어] 상위 32비트는 rand() shift, 하위는 rand() 그대로 — 총 64비트 채움
                                  *  - rand()가 15/31비트 범위일 수 있어 비트 일부는 항상 0, 분포 편향 있음 */

	/* Avoid zero seed */
	if (seed == 0) {
		seed = 1;                /* [한국어] 극히 드물지만 0이 나오면 고정 폴백 값 */
	}

	return seed;
}

static inline uint32_t
spdk_align32pow2(uint32_t x)
/*
 * [한국어]
 * spdk_align32pow2 - x 이상인 가장 작은 2의 거듭제곱 (32비트)
 *
 * 예: 1000 → 1024, 1024 → 1024, 1025 → 2048.
 * 0 또는 2^31 초과 시 동작 미정의 (spdk_u32log2(x-1)에 의존).
 */
{
	return 1u << (1 + spdk_u32log2(x - 1));
                                 /* [한국어] floor(log2(x-1))+1 → ceil(log2(x)) */
}

uint64_t spdk_u64log2(uint64_t x);
/*
 * [한국어]
 * spdk_u64log2 - floor(log2(x)) 반환 (64비트)
 */

static inline uint64_t
spdk_align64pow2(uint64_t x)
/*
 * [한국어]
 * spdk_align64pow2 - 64비트 버전 align pow2
 */
{
	return 1ULL << (1 + spdk_u64log2(x - 1));
}

/**
 * Check if a uint32_t is a power of 2.
 */
static inline bool
spdk_u32_is_pow2(uint32_t x)
/*
 * [한국어]
 * spdk_u32_is_pow2 - x가 2의 거듭제곱인지 (x>0)
 */
{
	if (x == 0) {
		return false;            /* [한국어] 0은 2^k으로 표현 불가 */
	}

	return (x & (x - 1)) == 0;   /* [한국어] 비트트릭: pow2이면 정확히 하나의 비트만 1 → x & (x-1) = 0 */
}

/**
 * Check if a uint64_t is a power of 2.
 */
static inline bool
spdk_u64_is_pow2(uint64_t x)
/*
 * [한국어]
 * spdk_u64_is_pow2 - 64비트 버전
 */
{
	if (x == 0) {
		return false;
	}

	return (x & (x - 1)) == 0;
}

static inline uint64_t
spdk_divide_round_up(uint64_t num, uint64_t divisor)
/*
 * [한국어]
 * spdk_divide_round_up - ceil(num/divisor) (64비트)
 * 사용처: 블록 수 계산, 전송 단위 청크 수 산출 등
 */
{
	return (num + divisor - 1) / divisor;
}

static inline uint64_t
spdk_round_up(uint64_t num, uint64_t divisor)
/*
 * [한국어]
 * spdk_round_up - num을 divisor의 배수로 올림
 */
{
	return divisor * spdk_divide_round_up(num, divisor);
}

struct spdk_single_ioviter {
	struct iovec	*iov;        /* [한국어] 이 iovec 배열의 시작 포인터 (호출자 소유) */
	size_t		iovcnt;          /* [한국어] 배열 원소 개수 */
	size_t		idx;             /* [한국어] 현재 순회 중인 인덱스 */
	size_t		iov_len;         /* [한국어] 현재 세그먼트에 남은 바이트 */
	uint8_t		*iov_base;       /* [한국어] 현재 세그먼트에서 아직 처리되지 않은 시작 포인터 */
};

/**
 * An N-way iovec iterator. Calculate the size, given N, using
 * SPDK_IOVITER_SIZE. For backward compatibility, the structure
 * has a default size of 2 iovecs.
 */
struct spdk_ioviter {
	uint32_t	count;           /* [한국어] 동시 순회하는 iovec의 수(N).
                                  *  - 2-way일 때는 count=2. N-way일 때 iters[]에 N개 slot 이어짐 */

	union {
		struct spdk_single_ioviter iters_compat[2];
                                 /* [한국어] 2-way 전용 구버전 ABI 호환 슬롯 */
		struct spdk_single_ioviter iters[0];
                                 /* [한국어] 가변 길이 배열(flexible array) — SPDK_IOVITER_SIZE 매크로로 N에 맞춰 할당 */
	};
};

/* count must be greater than or equal to 2 */
#define SPDK_IOVITER_SIZE(count) (sizeof(struct spdk_single_ioviter) * (count - 2) + sizeof(struct spdk_ioviter))
                                 /* [한국어] N-way iter 전체 크기 = 기본 구조체(2슬롯 포함) + 추가 (N-2)슬롯 */

/**
 * Initialize and move to the first common segment of the two given
 * iovecs. See spdk_ioviter_next().
 */
size_t spdk_ioviter_first(struct spdk_ioviter *iter,
			  struct iovec *siov, size_t siovcnt,
			  struct iovec *diov, size_t diovcnt,
			  void **src, void **dst);
/*
 * [한국어]
 * spdk_ioviter_first - 2-way iovec 순회 초기화 + 첫 공통 세그먼트 획득
 *
 * @iter: 사용자가 할당한 iter 객체
 * @siov/@siovcnt: source iovec 배열
 * @diov/@diovcnt: dest iovec 배열
 * @src/@dst: 현재 공통 세그먼트의 포인터 반환 (OUT)
 * @return: 현재 공통 세그먼트 바이트 수. 0이면 둘 다 끝.
 *
 * 사용처: 서로 다른 분할의 두 iovec을 동일 바이트 범위로 병행 처리
 */

/**
 * Initialize and move to the first common segment of the N given
 * iovecs. See spdk_ioviter_nextv().
 */
size_t spdk_ioviter_firstv(struct spdk_ioviter *iter,
			   uint32_t count,
			   struct iovec **iov,
			   size_t *iovcnt,
			   void **out);
/*
 * [한국어]
 * spdk_ioviter_firstv - N-way 버전 first
 *
 * @count: N (>=2)
 * @iov: N개 iovec* 배열
 * @iovcnt: 각 iovec의 길이 배열 (size_t[count])
 * @out: 각 iovec의 현재 세그먼트 포인터 배열 (void*[count])
 */

/**
 * Move to the next segment in the iterator.
 *
 * This will iterate through the segments of the source and destination
 * and return the individual segments, one by one. For example, if the
 * source consists of one element of length 4k and the destination
 * consists of 4 elements each of length 1k, this function will return
 * 4 1k src+dst pairs of buffers, and then return 0 bytes to indicate
 * the iteration is complete on the fifth call.
 */
size_t spdk_ioviter_next(struct spdk_ioviter *iter, void **src, void **dst);
/*
 * [한국어]
 * spdk_ioviter_next - 다음 공통 세그먼트로 이동 (2-way)
 * return 0이 순회 종료 신호.
 */

/**
 * Move to the next segment in the iterator.
 *
 * This will iterate through the segments of the iovecs in the iterator
 * and return the individual segments, one by one. For example, if the
 * set consists one iovec of one element of length 4k and another iovec
 * of 4 elements each of length 1k, this function will return
 * 4 1k pairs of buffers, and then return 0 bytes to indicate
 * the iteration is complete on the fifth call.
 */
size_t spdk_ioviter_nextv(struct spdk_ioviter *iter, void **out);
/*
 * [한국어]
 * spdk_ioviter_nextv - 다음 공통 세그먼트로 이동 (N-way)
 */

/**
 * Operate like memset across an iovec.
 */
void
spdk_iov_memset(struct iovec *iovs, int iovcnt, int c);
/*
 * [한국어]
 * spdk_iov_memset - iovec 전체에 c 값 채우기 (memset 동등)
 */

/**
 * Initialize an iovec with just the single given buffer.
 */
#define SPDK_IOV_ONE(piov, piovcnt, buf, buflen) do {	\
	(piov)->iov_base = (buf);			\
	(piov)->iov_len = (buflen);			\
	*(piovcnt) = 1;					\
} while (0)
                                 /* [한국어] 단일 버퍼 iovec 초기화 보일러플레이트 제거 매크로 */

/**
 * Copy the data described by the source iovec to the destination iovec.
 *
 * \return The number of bytes copied.
 */
size_t spdk_iovcpy(struct iovec *siov, size_t siovcnt, struct iovec *diov, size_t diovcnt);
/*
 * [한국어]
 * spdk_iovcpy - 두 iovec 간 데이터 복사 (비중첩 가정)
 */

/**
 * Same as spdk_iovcpy(), but the src/dst buffers might overlap.
 *
 * \return The number of bytes copied.
 */
size_t spdk_iovmove(struct iovec *siov, size_t siovcnt, struct iovec *diov, size_t diovcnt);
/*
 * [한국어]
 * spdk_iovmove - spdk_iovcpy 동등이지만 메모리 중첩 허용 (memmove 의미론)
 */

/**
 * Transfer state for iterative copying in or out of an iovec.
 */
struct spdk_iov_xfer {
	struct iovec *iovs;          /* [한국어] 대상 iovec 배열 */
	int iovcnt;                  /* [한국어] 배열 길이 */
	int cur_iov_idx;             /* [한국어] 현재 순회 인덱스 */
	size_t cur_iov_offset;       /* [한국어] 현재 세그먼트 내 바이트 오프셋 */
};

/**
 * Initialize a transfer context to point to the given iovec.
 */
void
spdk_iov_xfer_init(struct spdk_iov_xfer *ix, struct iovec *iovs, int iovcnt);
/*
 * [한국어]
 * spdk_iov_xfer_init - iov_xfer를 iovec 시작 위치에서 초기화
 */

/**
 * Copy from the given buf up to buf_len bytes, into the given ix iovec
 * iterator, advancing the iterator as needed.. Returns the number of bytes
 * copied.
 */
size_t
spdk_iov_xfer_from_buf(struct spdk_iov_xfer *ix, const void *buf, size_t buf_len);
/*
 * [한국어]
 * spdk_iov_xfer_from_buf - buf를 ix가 가리키는 iovec 위치에 복사(반복 가능)
 * iter 진행 포인터가 자동 전진 — 여러 번 호출해 점진적 복사 가능
 */

/**
 * Copy from the given ix iovec iterator into the given buf up to buf_len
 * bytes, advancing the iterator as needed. Returns the number of bytes copied.
 */
size_t
spdk_iov_xfer_to_buf(struct spdk_iov_xfer *ix, const void *buf, size_t buf_len);
/*
 * [한국어]
 * spdk_iov_xfer_to_buf - ix 위치에서 buf로 복사
 * @buf 인자가 const void*로 선언된 것은 역사적 API 실수 — 실제로는 쓰기 대상
 */

/**
 * Copy iovs contents to buf through memcpy.
 */
void spdk_copy_iovs_to_buf(void *buf, size_t buf_len, struct iovec *iovs,
			   int iovcnt);
/*
 * [한국어]
 * spdk_copy_iovs_to_buf - iovec 전체를 단일 buf로 평탄화 복사
 */

/**
 * Copy buf contents to iovs through memcpy.
 */
void spdk_copy_buf_to_iovs(struct iovec *iovs, int iovcnt, void *buf,
			   size_t buf_len);
/*
 * [한국어]
 * spdk_copy_buf_to_iovs - 단일 buf를 iovec 배열로 분산 복사
 */

/**
 * Scan build is really pessimistic and assumes that mempool functions can
 * dequeue NULL buffers even if they return success. This is obviously a false
 * positive, but the mempool dequeue can be done in a DPDK inline function that
 * we can't decorate with usual assert(buf != NULL). Instead, we'll
 * preinitialize the dequeued buffer array with some dummy objects.
 */
#define SPDK_CLANG_ANALYZER_PREINIT_PTR_ARRAY(arr, arr_size, buf_size) \
	do { \
		static char dummy_buf[buf_size]; \
		int i; \
		for (i = 0; i < arr_size; i++) { \
			arr[i] = (void *)dummy_buf; \
		} \
	} while (0)
                                 /* [한국어] scan-build 오탐 억제용 — DPDK mempool dequeue가 NULL을 반환한다고 오판하는 것을 더미 포인터로 무력화
                                  *  - 일반 빌드에서는 dead store로 최적화되어 런타임 비용 미미 */

/**
 * Add two sequence numbers s1 and s2
 *
 * \param s1 First sequence number
 * \param s2 Second sequence number
 *
 * \return Sum of s1 and s2 based on serial number arithmetic.
 */
static inline uint32_t
spdk_sn32_add(uint32_t s1, uint32_t s2)
/*
 * [한국어]
 * spdk_sn32_add - 32비트 serial number 덧셈 (RFC 1982 style)
 * 일반 uint32 덧셈과 동일하나 "시퀀스 번호" 의도를 명시. wrap-around 의미 보존
 */
{
	return (uint32_t)(s1 + s2);
}

#define SPDK_SN32_CMPMAX	(1U << (32 - 1))
                                 /* [한국어] 2^31 — 시퀀스 공간 절반. 이 값보다 큰 차이는 "wrap-around된 반대편"으로 해석 */

/**
 * Compare if sequence number s1 is less than s2.
 *
 * \param s1 First sequence number
 * \param s2 Second sequence number
 *
 * \return true if s1 is less than s2, or false otherwise.
 */
static inline bool
spdk_sn32_lt(uint32_t s1, uint32_t s2)
/*
 * [한국어]
 * spdk_sn32_lt - wrap-around 고려 less-than (RFC 1982 §3.2)
 *
 * 일반 s1 < s2는 wrap-around 발생 시 오답. 이 함수는 차이가 2^31 미만일 때만
 * 원래 순서를 믿고, 그 이상이면 반대로 해석한다.
 */
{
	return (s1 != s2) &&
	       ((s1 < s2 && s2 - s1 < SPDK_SN32_CMPMAX) ||
		(s1 > s2 && s1 - s2 > SPDK_SN32_CMPMAX));
                                 /* [한국어] case1: s1<s2이고 차이<2^31 → 보통 less-than
                                  *  case2: s1>s2인데 차이>2^31 → wrap된 less-than (s1이 반대편에서 앞서감) */
}

/**
 * Compare if sequence number s1 is greater than s2.
 *
 * \param s1 First sequence number
 * \param s2 Second sequence number
 *
 * \return true if s1 is greater than s2, or false otherwise.
 */
static inline bool
spdk_sn32_gt(uint32_t s1, uint32_t s2)
/*
 * [한국어]
 * spdk_sn32_gt - wrap-around 고려 greater-than
 */
{
	return (s1 != s2) &&
	       ((s1 < s2 && s2 - s1 > SPDK_SN32_CMPMAX) ||
		(s1 > s2 && s1 - s2 < SPDK_SN32_CMPMAX));
                                 /* [한국어] lt의 대칭 로직 */
}

/**
 * Copies the value (unsigned char)ch into each of the first \b count characters of the object pointed to by \b data
 * \b data_size is used to check that filling \b count bytes won't lead to buffer overflow
 *
 * \param data Buffer to fill
 * \param data_size Size of the buffer
 * \param ch Fill byte
 * \param count Number of bytes to fill
 */
static inline void
spdk_memset_s(void *data, size_t data_size, int ch, size_t count)
/*
 * [한국어]
 * spdk_memset_s - 안전 memset (dead store 최적화 방지 + 경계 검사)
 *
 * 일반 memset은 "사용되지 않는" 메모리 클리어를 옵티마이저가 삭제할 수 있어
 * 민감 데이터(암호키 등)가 메모리에 잔존할 수 있다. C11 Annex K의 memset_s나
 * volatile 포인터 경유 루프로 이를 방지한다.
 * data_size 경계 체크로 오버플로도 방지.
 */
{
#ifdef __STDC_LIB_EXT1__
	/* memset_s was introduced as an optional feature in C11 */
	memset_s(data, data_size, ch, count);
                                 /* [한국어] Annex K 지원 libc면 표준 함수로 위임 — 컴파일러/라이브러리 보장으로 삭제 불가 */
#else
	size_t i;
	volatile unsigned char *buf = (volatile unsigned char *)data;
                                 /* [한국어] volatile 포인터 → 옵티마이저가 쓰기 루프를 삭제하지 못함 */

	if (!buf) {
		return;                  /* [한국어] NULL 가드 */
	}
	if (count > data_size) {
		count = data_size;       /* [한국어] 범위 클램프 — 오버플로 방지 */
	}

	for (i = 0; i < count; i++) {
		buf[i] = (unsigned char)ch;
                                 /* [한국어] 바이트 단위 쓰기 — volatile 덕분에 컴파일러가 memset 호출로 다시 바꿔 최적화해도 삭제는 안 됨 */
	}
#endif
}

#ifdef __cplusplus
}
#endif

#endif                           /* [한국어] include 가드 종료 */
