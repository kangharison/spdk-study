/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] SPDK Deprecation 추적기 (log_deprecated.c)
 *
 * === 파일의 역할 ===
 * SPDK가 사용자에게 "이 기능/API는 곧 사라질 예정이다"라고 알리는 deprecation 경고
 * 인프라를 제공한다. 같은 deprecated 코드 경로가 N회 fire되면 매번 경고를 찍는 대신,
 * (1) 누적 hits 카운터를 증가시키고, (2) 사용자가 정한 rate-limit 간격(초) 동안에는
 * 경고를 억제한 뒤, (3) 간격이 지나면 한 번 출력하면서 그 사이 억제된 메시지 개수를
 * 함께 알려준다. 또한 RPC `log_get_deprecation_history`가 이 추적기를 순회해 모든
 * deprecation의 tag/description/remove_release/hits를 JSON으로 덤프한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SPDK 내 어디에서든 SPDK_LOG_DEPRECATED(dep_handle) 매크로를 통해 사용된다:
 *   - 컴포넌트 초기화 시 spdk_log_deprecation_register("tag", "desc", "25.05", 60, &dep)
 *     로 핸들을 등록(rate_limit_seconds=60이면 같은 deprecation은 60초에 1회만 출력).
 *   - 핫패스에서 SPDK_LOG_DEPRECATED(dep)을 호출하면 spdk_log_deprecated가 hits 카운터를
 *     증가시키고 (rate_limit 통과 시) spdk_log(SPDK_LOG_WARN, ...)으로 경고 출력.
 * 호출 체인:
 *   - 등록:    초기화 코드 → spdk_log_deprecation_register → calloc + TAILQ_INSERT
 *   - fire:    SPDK_LOG_DEPRECATED → spdk_log_deprecated → (rate-limit 검사) → spdk_log
 *   - RPC 조회: rpc_log_get_deprecation_history → spdk_log_for_each_deprecation
 *              → 사용자 콜백(JSON 직렬화)
 * 실행 컨텍스트: 호스트 유저스페이스. fire는 어떤 reactor/lcore에서든 일어날 수 있어
 * race가 가능 — SPDK는 deprecation 카운팅의 정확성보다 성능을 우선해 락 없이 단순 ++로
 * 처리한다(아래 spdk_log_deprecated 주석 참조).
 *
 * === 타 모듈과의 연결 ===
 * - 의존: libc <time.h>(clock_gettime, CLOCK_MONOTONIC), <stdlib.h>(calloc/free),
 *         <string.h>(strnlen), <stdio.h>(snprintf). SPDK 내부로는 SPDK_SEC_TO_NSEC
 *         (spdk/util.h), SPDK_ERRLOG/WARNLOG/spdk_log (spdk/log.h).
 * - 의존되는 쪽: SPDK 곳곳의 deprecated API/구조체에서 SPDK_LOG_DEPRECATED 매크로 사용.
 *               module/event/rpc 의 RPC `log_get_deprecation_history` 핸들러가
 *               spdk_log_for_each_deprecation을 호출해 결과를 JSON으로 변환.
 * - 공유 상태: g_deprecations(TAILQ 헤드 — deprecation 핸들 리스트),
 *             g_deprecation_epoch(constructor에서 잡은 시작 시각, 간격 계산 기준).
 *
 * === 주요 함수/구조체 요약 ===
 *   - struct spdk_deprecation : 한 deprecation 단위. tag(고유 식별), desc(설명),
 *                               remove(제거 예정 릴리스), hits(누적 fire 수), interval
 *                               (rate-limit, ns), deferred(억제된 횟수), last_log(직전
 *                               출력 시각, ns since epoch).
 *   - deprecation_init() : __attribute__((constructor)) — main() 진입 전에 epoch 기록.
 *   - get_ns_since_epoch() : 현재 시각을 epoch 기준 나노초로 반환(rate-limit 비교용).
 *   - spdk_log_deprecation_register() : 새 deprecation 핸들을 calloc + 리스트에 등록.
 *   - spdk_log_deprecated() : ★ 핵심 — fire 1회를 hits++, rate-limit 검사, 통과 시
 *                             spdk_log(WARN)로 출력 + deferred 누적치도 함께 보고.
 *   - spdk_log_for_each_deprecation() : RPC가 사용하는 이터레이터.
 *   - spdk_deprecation_get_tag/desc/remove_release/hits : RPC가 사용하는 read-only getter.
 */

#include "spdk/stdinc.h"
/* [한국어] 표준 인클루드 묶음 — <time.h>(clock_gettime/CLOCK_MONOTONIC), <stdlib.h>
 * (calloc/free), <string.h>(strnlen), <stdio.h>(snprintf), <errno.h>(ENOMEM), <stdint.h>,
 * <assert.h>(assert) 등을 한 번에 가져온다. */
#include "spdk/string.h"
/* [한국어] 안전 문자열 헬퍼 — 본 파일에서는 직접 호출은 적지만, deprecation 메시지에서
 * 사용하는 snprintf 등을 보강하는 SPDK 유틸이 정의된 헤더이므로 일관성 차원에서 포함. */
#include "spdk/thread.h"
/* [한국어] spdk_thread API — 본 파일은 직접 spdk_thread를 쓰진 않지만, spdk_log_deprecated
 * 호출 시점이 spdk_thread 컨텍스트에 있을 가능성을 알리고, 향후 spdk_get_thread() 같은
 * 진단 추가를 대비해 인클루드. */
#include "spdk/util.h"
/* [한국어] SPDK_SEC_TO_NSEC 매크로(=10억) 사용을 위한 헤더 — interval(초→나노초) 변환과
 * epoch 시각 차이 계산에 필요. */
#include "spdk/log.h"
/* [한국어] spdk_log/SPDK_LOG_WARN/SPDK_ERRLOG/SPDK_WARNLOG, struct spdk_deprecation
 * 전방 선언, spdk_log_for_each_deprecation_fn 콜백 타입을 가져옴. */

struct spdk_deprecation {
	/* [한국어] 한 deprecation의 모든 상태를 담는 구조체. SPDK_LOG_DEPRECATED가 동작하기
	 * 위한 메모리 단위 — calloc으로 동적 할당되어 g_deprecations TAILQ에 연결됨. */

	const char tag[64];
	/* [한국어] deprecation 고유 식별자 — 짧은 영문 키워드 (예: "old_bdev_register").
	 * 설정자: spdk_log_deprecation_register()가 snprintf로 채움.
	 * 읽는 자: spdk_log_deprecated가 경고 메시지에 박을 때, RPC 응답 직렬화 시.
	 * 값 범위: 63자 이하 ASCII (NUL 포함 64자). const char로 선언하지만 실제로 strncpy
	 *         스타일 초기화를 위해 (char*) 캐스팅으로 한 번 쓴다 — register 함수 참조. */

	const char desc[64];
	/* [한국어] 사람이 읽는 설명 — "old foo_init() function" 같은 1줄 설명.
	 * 설정자: register에서 채움. 읽는 자: 출력 메시지/RPC.
	 * 값 범위: 63자 이하. */

	const char remove[16];
	/* [한국어] 제거 예정 릴리스 문자열 — "25.05" 같은 SPDK 분기 버전.
	 * 설정자: register. 읽는 자: 출력 메시지/RPC.
	 * 값 범위: 15자 이하. */

	TAILQ_ENTRY(spdk_deprecation) link;
	/* [한국어] g_deprecations TAILQ에 연결되는 prev/next 포인터 쌍.
	 * 설정자: TAILQ_INSERT_TAIL(register에서 호출). 읽는 자: TAILQ_FOREACH.
	 * 동기화: register는 단일 컨텍스트에서 일어나므로 락 불필요. */

	uint64_t hits;
	/* [한국어] 누적 fire 횟수 — spdk_log_deprecated가 호출될 때마다 ++.
	 * 설정자: spdk_log_deprecated(여러 lcore에서 동시).
	 * 읽는 자: spdk_deprecation_get_hits (RPC 응답).
	 * 값 범위: 0 ~ UINT64_MAX (사실상 오버플로우 안 일어남).
	 * 동기화: race로 일부 증가가 누락될 수 있음 — 정확성보다 성능 우선(파일 상단 주석 참조). */

	/* How often (nanoseconds) to log. */
	uint64_t interval;
	/* [한국어] rate-limit 간격(나노초). 같은 deprecation의 두 출력 사이 최소 시간.
	 * 설정자: register에서 rate_limit_seconds * SPDK_SEC_TO_NSEC.
	 * 읽는 자: spdk_log_deprecated의 시간 비교.
	 * 값 범위: 0(=억제 없음, 매번 출력) ~ 매우 큰 값. 동기화: read-only after register. */

	/* How many messages were not logged due to rate limiting */
	uint32_t deferred;
	/* [한국어] rate-limit으로 억제된 메시지 개수. 다음 출력 시 함께 보고 후 0으로 리셋.
	 * 설정자: spdk_log_deprecated(억제 시 ++, 출력 시 0으로). 읽는 자: 출력 코드.
	 * 값 범위: 0 ~ UINT32_MAX(현실적으론 사용자가 그 사이에 출력함).
	 * 동기화: race 시 일부 카운트 손실 가능 — 허용. */

	/* CLOCK_MONOTONIC microseconds since g_deprecation_epoch when last warning was logged */
	uint64_t last_log;
	/* [한국어] 직전 출력 시각(epoch 기준 ns). 다음 호출에서 (now - last_log) >= interval
	 * 인지 검사해 출력 여부 결정.
	 * 설정자: spdk_log_deprecated(출력 직후 now로 갱신).
	 * 읽는 자: 같은 함수의 시간 비교.
	 * 값 범위: 0(아직 한 번도 출력 안 됨) ~ 현재 epoch 기준 ns.
	 * 동기화: race 시 두 lcore가 거의 동시에 fire하면 둘 다 출력 가능 — 약간의 중복 허용. */
};

static TAILQ_HEAD(, spdk_deprecation) g_deprecations = TAILQ_HEAD_INITIALIZER(g_deprecations);
/* [한국어] 등록된 모든 deprecation 핸들의 전역 TAILQ 헤드.
 * 설정자: spdk_log_deprecation_register(보통 부트스트랩 단계).
 * 읽는 자: spdk_log_for_each_deprecation(RPC 핸들러).
 * 동기화: 등록은 단일 스레드 부트스트랩에서 일어나므로 구조 변경 race 없음. */
static struct timespec g_deprecation_epoch;
/* [한국어] 시간 차 계산의 기준점 — constructor에서 한 번 잡힌 CLOCK_MONOTONIC 시작점.
 * MONOTONIC을 쓰는 이유: NTP/사용자에 의한 시계 점프에 영향받지 않아 rate-limit이 안정적.
 * 설정자: deprecation_init(constructor, main 전 1회). 읽는 자: get_ns_since_epoch.
 * 값 범위: 시스템 부팅 후 임의의 시점. 동기화: read-only after constructor. */

/*
 * [한국어]
 * deprecation_init - constructor 단계에서 epoch 시각을 한 번 잡는다.
 *
 * @return: 없음.
 *
 * __attribute__((constructor))로 main() 진입 전에 자동 호출된다. CLOCK_MONOTONIC을
 * 사용하므로 시스템 부팅 후 임의의 시점이 epoch가 되며, 이후 모든 시간 차이는 이 값을
 * 빼서 ns 단위로 표현된다(즉 g_deprecation_epoch 자체의 절대값은 의미 없음, 차이만 사용).
 *
 * 호출 체인:
 *   (constructor) → deprecation_init → clock_gettime(CLOCK_MONOTONIC)
 */
static void
__attribute__((constructor))
deprecation_init(void)
{
	clock_gettime(CLOCK_MONOTONIC, &g_deprecation_epoch);
	/* [한국어] CLOCK_MONOTONIC은 시스템 wall-clock 변경(NTP/관리자)에도 단조 증가만 함.
	 * rate-limit 비교에 적합 — 점프하지 않아 갑자기 "이 deprecation 다시 출력해야"가
	 * 트리거되거나 영원히 억제되는 일을 방지. */
}

/*
 * [한국어]
 * get_ns_since_epoch - g_deprecation_epoch 기준 현재 시각을 ns 단위로 반환.
 *
 * @return: 64비트 ns 차이값 (대략 584년 분량 표현 가능 — 오버플로우 무시).
 *
 * spdk_log_deprecated가 dep->last_log + dep->interval 비교에 쓸 통일된 시각 표현.
 * 64비트로 ns를 나타내므로 음수 미발생.
 *
 * 호출 체인:
 *   spdk_log_deprecated → get_ns_since_epoch → clock_gettime + 차분 계산
 */
static inline uint64_t
get_ns_since_epoch(void)
{
	struct timespec now;
	/* [한국어] 현재 MONOTONIC 시각을 받을 임시 변수. */

	clock_gettime(CLOCK_MONOTONIC, &now);
	/* [한국어] 시각 가져오기 — VDSO 구현이 있는 시스템에서는 syscall 없이 빠르게 동작. */
	return (now.tv_sec - g_deprecation_epoch.tv_sec) * SPDK_SEC_TO_NSEC +
	       now.tv_nsec - g_deprecation_epoch.tv_nsec;
	/* [한국어] (초 차이 * 10^9) + 나노초 차이 = 총 ns 차이.
	 * SPDK_SEC_TO_NSEC = 1000000000ULL. tv_nsec 차이는 음수가 될 수 있지만 sec 보정과
	 * 함께 하면 결과는 항상 양수가 된다(now > epoch 보장 — MONOTONIC 단조 증가). */
}

/*
 * [한국어]
 * spdk_log_deprecation_register - 새 deprecation 핸들을 동적 할당해 g_deprecations에 등록.
 *
 * @tag:                 짧은 고유 식별자 (예: "old_bdev_register"). 63자 이하.
 * @description:         사람이 읽는 1줄 설명. 63자 이하.
 * @remove_release:      제거 예정 릴리스 문자열 (예: "25.05"). 15자 이하.
 * @rate_limit_seconds:  rate-limit 간격(초). 0이면 매 fire마다 출력.
 * @depp:                결과 핸들을 받을 포인터의 포인터. 성공 시 새 핸들이 *depp에 저장.
 * @return: 0 성공, -ENOMEM(calloc 실패).
 *
 * 일반적으로 컴포넌트 초기화 시 한 번 호출되어 정적 핸들 변수에 저장한 뒤, 핫패스에서
 * SPDK_LOG_DEPRECATED(dep)로 fire한다.
 * 메모리: calloc으로 할당된 핸들은 free되지 않음 — 프로세스 수명 동안 유지된다(사실상 leak
 *         이지만, 핸들 수가 적고 종료 시 OS가 회수하므로 문제 없음).
 *
 * 호출 체인:
 *   초기화 코드 → spdk_log_deprecation_register → calloc + snprintf + TAILQ_INSERT_TAIL
 */
int
spdk_log_deprecation_register(const char *tag, const char *description, const char *remove_release,
			      uint32_t rate_limit_seconds, struct spdk_deprecation **depp)
{
	struct spdk_deprecation *dep;
	/* [한국어] 새로 할당할 핸들 포인터. */

	assert(strnlen(tag, sizeof(dep->tag)) < sizeof(dep->tag));
	/* [한국어] tag 길이 검사 — strnlen은 buffer overrun 없이 길이 측정. NUL 포함 64자
	 * 이내인지 확인(<은 NUL을 위한 1자 여유). 잘못된 호출은 디버그 빌드에서 abort. */
	assert(strnlen(description, sizeof(dep->desc)) < sizeof(dep->desc));
	/* [한국어] description도 동일 검사. */
	assert(strnlen(remove_release, sizeof(dep->remove)) < sizeof(dep->remove));
	/* [한국어] remove_release도 동일 검사. */

	dep = calloc(1, sizeof(*dep));
	/* [한국어] 0으로 초기화된 핸들 1개 할당 — hits/deferred/last_log 모두 0으로 시작.
	 * link 필드도 0(=NULL)이 되므로 TAILQ_INSERT 전 안전. */
	if (dep == NULL) {
		return -ENOMEM;
		/* [한국어] OOM — 호출자에게 -ENOMEM 반환. 흔치 않은 경로(64+64+16+여러 필드 = 160B 정도). */
	}

	snprintf((char *)dep->tag, sizeof(dep->tag), "%s", tag);
	/* [한국어] tag를 핸들 내 const char[64] 버퍼에 복사. const 캐스트는 필드 정의가 const라
	 * "할당 후 변경 금지"를 표현하는 의도이지만, 첫 초기화는 예외적으로 허용. snprintf는
	 * 버퍼 초과 시 자동으로 잘라 NUL 종결 보장. */
	snprintf((char *)dep->desc, sizeof(dep->desc), "%s", description);
	/* [한국어] description 동일 방식 복사. */
	snprintf((char *)dep->remove, sizeof(dep->remove), "%s", remove_release);
	/* [한국어] remove_release 동일 방식 복사. */
	dep->interval = rate_limit_seconds * SPDK_SEC_TO_NSEC;
	/* [한국어] 초 → 나노초 변환. 0이면 0이 그대로 들어가 spdk_log_deprecated의 if
	 * (interval != 0) 분기에서 억제 로직을 건너뛰게 됨(=매번 출력). */

	TAILQ_INSERT_TAIL(&g_deprecations, dep, link);
	/* [한국어] 등록 순서대로 끝에 삽입 — 정렬은 안 함. RPC가 순회할 때 등록 순으로 출력됨. */
	*depp = dep;
	/* [한국어] 호출자에게 핸들 반환 — 이후 SPDK_LOG_DEPRECATED(dep)에서 사용. */
	return 0;
}

/*
 * There is potential for races between pthreads leading to over or under reporting of times that
 * deprecated code was hit. If this function is called in a hot path where that is likely, we care
 * more about performance than accuracy of the error counts. The important thing is that at least
 * one of the racing updates the hits counter to non-zero and the warning is logged at least once.
 */
/*
 * [한국어]
 * spdk_log_deprecated - ★ 핵심 — deprecated 코드 경로 1회 fire를 기록하고 (rate-limit
 *                        통과 시) 사용자에게 경고를 출력한다.
 *
 * @dep:  spdk_log_deprecation_register로 미리 등록된 핸들. NULL이면 ERRLOG + assert.
 * @file: __FILE__ — fire한 호출 위치 파일명.
 * @line: __LINE__.
 * @func: __func__.
 * @return: 없음.
 *
 * 동작:
 *   1) 현재 시각(now) 측정.
 *   2) NULL 핸들 가드(잘못된 호출 — 빠른 실패).
 *   3) hits++로 누적 카운터 증가 (race 허용).
 *   4) interval != 0이면 rate-limit 검사:
 *      - 직전 출력 이후 interval 경과 안 됨 → deferred++ 후 return(억제).
 *   5) 통과: last_log = now로 갱신, spdk_log(WARN)로 사용자 경고 출력.
 *   6) deferred > 0이면 그 사이 억제된 메시지 수도 함께 보고 후 0으로 리셋.
 *
 * 동시성: 여러 lcore가 같은 dep를 동시에 fire할 수 있지만, 본 함수는 일부러 락을 쓰지
 * 않는다. race로 hits 카운트가 1~수회 누락되거나, 거의 동시 fire 시 출력이 2회 발생할
 * 수 있지만, deprecation 경고의 목적상 "최소 1회 사용자에게 알린다"만 보장되면 충분하다
 * (위 영문 주석 참조).
 *
 * 호출 체인:
 *   SPDK_LOG_DEPRECATED 매크로 → spdk_log_deprecated → spdk_log(SPDK_LOG_WARN)
 *                              → (선택) SPDK_WARNLOG("messages suppressed")
 */
void
spdk_log_deprecated(struct spdk_deprecation *dep, const char *file, uint32_t line, const char *func)
{
	uint64_t now = get_ns_since_epoch();
	/* [한국어] 현재 시각을 epoch 기준 ns로 — rate-limit 비교의 좌변. */

	if (dep == NULL) {
		/* [한국어] 잘못된 사용 가드 — 핸들을 등록하지 않고 호출했거나, 매크로 전개 오류 등. */
		SPDK_ERRLOG("NULL deprecation passed from %s:%u:%s\n", file, line, func);
		assert(false);
		/* [한국어] 디버그 빌드에선 abort, release에선 무시 후 안전 return. */
		return;
	}

	dep->hits++;
	/* [한국어] 누적 fire 카운터 — 락 없는 ++. race 시 일부 증가 누락 가능, 허용. */

	if (dep->interval != 0) {
		/* [한국어] rate-limit 활성 — 같은 deprecation의 출력 빈도를 제한. */
		if (dep->last_log != 0 && now < dep->last_log + dep->interval) {
			/* [한국어] last_log==0(처음 fire)이면 이 분기 안 들어가서 즉시 출력.
			 * 그렇지 않고 last_log + interval 시점이 아직 안 됐으면 출력 억제. */
			dep->deferred++;
			/* [한국어] 억제 카운트 — 다음 출력 시 함께 보고. race 허용. */
			return;
		}
	}

	dep->last_log = now;
	/* [한국어] 출력 시각 갱신 — race 시 약간 옛 값으로 덮일 수 있으나 큰 문제 없음. */

	spdk_log(SPDK_LOG_WARN, file, line, func, "%s: deprecated feature %s to be removed in %s\n",
		 dep->tag, dep->desc, dep->remove);
	/* [한국어] 사용자에게 deprecation 경고 출력 — WARN 레벨로 syslog/stderr에 기록.
	 * 메시지 형식: "<tag>: deprecated feature <desc> to be removed in <release>". */
	if (dep->deferred != 0) {
		/* [한국어] 그 사이 억제된 메시지가 있으면 함께 보고 — 사용자가 빈도를 인지하도록. */
		SPDK_WARNLOG("%s: %u messages suppressed\n", dep->tag, dep->deferred);
		dep->deferred = 0;
		/* [한국어] 카운터 리셋 — 다음 사이클부터 다시 카운팅. */
	}
}

/*
 * [한국어]
 * spdk_log_for_each_deprecation - 등록된 모든 deprecation을 순회하며 콜백 호출.
 *
 * @ctx: 사용자 컨텍스트 — 콜백에 그대로 전달.
 * @fn:  각 deprecation에 대해 호출될 콜백. 0이 아닌 값을 반환하면 순회 중단.
 * @return: 마지막 콜백 반환값(또는 모두 성공이면 0).
 *
 * RPC `log_get_deprecation_history` 핸들러가 사용 — 콜백이 deprecation 정보를 JSON으로
 * 직렬화해 응답에 누적한다. 콜백 안에서 spdk_deprecation_get_*() getter를 사용해 필드
 * 접근(외부에서 const struct spdk_deprecation은 불투명 타입).
 *
 * 호출 체인:
 *   RPC `log_get_deprecation_history` 핸들러 → spdk_log_for_each_deprecation
 *      → 콜백(JSON 직렬화) — TAILQ_FOREACH 순서대로(=등록 순서)
 */
int
spdk_log_for_each_deprecation(void *ctx, spdk_log_for_each_deprecation_fn fn)
{
	struct spdk_deprecation *dep;
	/* [한국어] 순회 커서. */
	int rc = 0;
	/* [한국어] 누적 반환값 — 콜백 실패 시 그 값을 즉시 반환. */

	TAILQ_FOREACH(dep, &g_deprecations, link) {
		rc = fn(ctx, dep);
		/* [한국어] 콜백 호출 — JSON 인코딩 등 사용자 책임. */
		if (rc != 0) {
			/* [한국어] 콜백이 실패 코드를 돌려주면 더 이상 순회하지 않고 즉시 중단. */
			break;
		}
	}

	return rc;
	/* [한국어] 마지막 콜백 결과 또는 모두 성공이면 0. */
}

/*
 * [한국어]
 * spdk_deprecation_get_tag - tag 필드 접근 게터 (RPC에서 const 핸들로부터 읽기 위해).
 *
 * @deprecation: const 핸들.
 * @return: NUL 종결된 tag 문자열 (수명: deprecation 핸들과 동일).
 *
 * 호출 체인: RPC 콜백 → spdk_deprecation_get_tag → (필드 read)
 */
const char *
spdk_deprecation_get_tag(const struct spdk_deprecation *deprecation)
{
	return deprecation->tag;
	/* [한국어] 단순 필드 반환 — const char[64] 배열의 첫 번째 요소 주소. */
}

/*
 * [한국어]
 * spdk_deprecation_get_description - description 게터.
 *
 * @deprecation: const 핸들.
 * @return: NUL 종결된 desc 문자열.
 *
 * 호출 체인: RPC 콜백 → spdk_deprecation_get_description
 */
const char *
spdk_deprecation_get_description(const struct spdk_deprecation *deprecation)
{
	return deprecation->desc;
	/* [한국어] desc 필드 반환. */
}

/*
 * [한국어]
 * spdk_deprecation_get_remove_release - remove_release 게터.
 *
 * @deprecation: const 핸들.
 * @return: NUL 종결된 remove 문자열 (예: "25.05").
 *
 * 호출 체인: RPC 콜백 → spdk_deprecation_get_remove_release
 */
const char *
spdk_deprecation_get_remove_release(const struct spdk_deprecation *deprecation)
{
	return deprecation->remove;
	/* [한국어] remove 필드 반환. */
}

/*
 * [한국어]
 * spdk_deprecation_get_hits - 누적 fire 카운트 게터.
 *
 * @deprecation: const 핸들.
 * @return: 현재까지의 hits (race로 약간 부정확할 수 있음 — 위 영문 주석 참조).
 *
 * 호출 체인: RPC 콜백 → spdk_deprecation_get_hits
 */
uint64_t
spdk_deprecation_get_hits(const struct spdk_deprecation *deprecation)
{
	return deprecation->hits;
	/* [한국어] hits 필드 반환. atomic 보장은 없지만 64-bit 대입은 일반적으로 atomic. */
}
