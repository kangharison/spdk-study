/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] 단일 생산자/단일 소비자 (SPSC) 원형 파이프 (pipe.c)
 *
 * === 파일의 역할 ===
 * 본 파일은 SPDK 가 사용하는 가벼운 원형(circular) 바이트 파이프(spdk_pipe)를
 * 구현한다. 핵심 사용처는 NVMe-oF TCP transport 의 socket recv/send 버퍼링
 * (lib/nvmf/tcp.c, lib/sock/...) 으로, recv 시스템 호출이 가져온 바이트들을
 * PDU 단위로 파싱해 내려보낼 때까지 잠시 보관하거나 send 측에서 미리 형성한
 * 페이로드를 전송할 때 사용된다. 또한 동일 reactor 의 여러 파이프가 한정된
 * 버퍼 풀(spdk_pipe_group)을 공유해 메모리 사용을 줄일 수 있도록 lazy buffer
 * 할당/회수 메커니즘도 포함되어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * lib/util 의 leaf 자료구조이며 단일 reactor(스레드) 내에서만 사용된다.
 *   producer (예: TCP recv loop) → spdk_pipe_writer_get_buffer / advance
 *   consumer (예: PDU 파서)     → spdk_pipe_reader_get_buffer / advance
 *
 * SPDK 의 thread affinity 모델 덕분에 같은 reactor 안에서 producer/consumer
 * 양쪽이 동일 스레드에서 직렬로 호출되는 패턴이 일반적이며(특히 TCP recv 후
 * 즉시 파서를 돌리는 경우), 그 결과 read/write/full 필드 갱신은 race 가
 * 발생하지 않는다 → lock-free 가능. 두 콜백이 다른 스레드에 있어도 SPSC
 * 의미를 유지하면 동작 가능하나, SPDK 의 일반적 사용 패턴은 단일 스레드.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: spdk/pipe.h(공개 API), spdk/util.h(spdk_min), spdk/queue.h(SLIST_*),
 *         spdk/log.h(SPDK_ERRLOG).
 * - 사용처: lib/nvmf/tcp.c, lib/sock/posix.c, lib/sock/uring.c 등의 recv 경로
 *           — 부분 PDU 의 누적 보관 및 재조립.
 * - 데이터 흐름: socket recv → writer_get_buffer 로 받은 iovs 에 readv →
 *   writer_advance 로 write 포인터 진행 → 파서가 reader_get_buffer 로 누적분
 *   확인 후 처리 → reader_advance 로 read 포인터 진행 → 빈 상태가 되면 buffer
 *   가 group 풀로 반환 (lazy).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spdk_pipe_buf  : group 의 free-list 노드 헤더 (SLIST 링크 + sz).
 * - struct spdk_pipe_group: SLIST 헤드만 가지는 공유 free 버퍼 풀.
 * - struct spdk_pipe      : circular buffer 본체 (buf, sz, write, read, full, group).
 * - spdk_pipe_create/destroy : 파이프 생성/해제 (외부 buffer 주입 모델).
 * - pipe_alloc_buf_from_group (static): group 풀에서 size 매칭 buffer 빌려옴.
 * - spdk_pipe_writer_get_buffer/advance : producer 측 인터페이스.
 * - spdk_pipe_reader_bytes_available/get_buffer/advance: consumer 측 인터페이스.
 * - spdk_pipe_group_create/destroy/add/remove : 파이프-그룹 관리.
 *
 * 각 buffer 의 끝에서 시작으로의 wrap-around 는 iovs[2] 두 조각으로 노출되며,
 * 호출자가 두 iov 모두 처리 가능 (readv/writev 와 자연스럽게 결합).
 */

/* [한국어] 공개 API 선언과 spdk_pipe / spdk_pipe_group 의 incomplete type 외부 노출. */
#include "spdk/pipe.h"
/* [한국어] spdk_min — get_buffer/advance 에서 wrap 분할 최소값 계산 시 사용. */
#include "spdk/util.h"
/* [한국어] BSD-style SLIST_* 매크로 — group 의 free buffer 단일 연결 리스트 구현. */
#include "spdk/queue.h"
/* [한국어] SPDK_ERRLOG — group 해제 시 누수 검출 진단 메시지. */
#include "spdk/log.h"

/* [한국어] free 상태 buffer 의 리스트 노드.
 *  - 사용 중 buffer 는 spdk_pipe 가 직접 보유 (pipe->buf == 이 구조체의 시작 주소).
 *  - 빈 상태 buffer 는 group 풀의 SLIST 에 push 되어 재사용을 기다림.
 *  - 따라서 buffer 의 첫 sizeof(spdk_pipe_buf) 바이트는 free 일 때 메타데이터로 점유됨.
 *    (사용 중에는 일반 데이터 영역으로 사용 가능 — 메타데이터는 link/sz 만이라 작음) */
struct spdk_pipe_buf {
	SLIST_ENTRY(spdk_pipe_buf)	link;
	/* [한국어] free-list 의 다음 노드를 가리키는 포인터.
	 *   설정자: spdk_pipe_group_add / spdk_pipe_reader_advance 가 reader 의 wrap 시
	 *           SLIST_INSERT_HEAD 로 설정.
	 *   읽는 자: pipe_alloc_buf_from_group 이 SLIST_REMOVE 로 풀에서 빼올 때.
	 *   값 범위: 같은 group 내의 다른 spdk_pipe_buf 또는 NULL.
	 *   동기화: 단일 스레드(같은 reactor)에서만 group 을 조작한다고 가정 → lock-free. */
	uint32_t			sz;
	/* [한국어] 이 buffer 의 총 크기(바이트). pipe 가 다시 할당받을 때 size 매칭에 사용.
	 *   설정자: pipe_destroy/reader_advance 등에서 buffer 를 풀로 반환하기 직전에 기록.
	 *   읽는 자: pipe_alloc_buf_from_group — 요청 size 와 정확히 같은 buffer 를 찾기 위해 비교.
	 *   값 범위: pipe 생성 시 호출자가 준 sz. */
};

/* [한국어] 여러 spdk_pipe 가 빈 buffer 를 공유하기 위한 풀.
 *  단일 reactor 내에서 동시에 활성화된 pipe 는 일부에 불과하다는 통계적
 *  사실을 이용해 메모리 점유를 줄이는 디자인이다. */
struct spdk_pipe_group {
	SLIST_HEAD(, spdk_pipe_buf) bufs;
	/* [한국어] 빈 buffer 의 free-list 헤드.
	 *   설정자: 새 group 생성(create) 시 SLIST_INIT, add/reader_advance 시 push.
	 *   읽는 자: pipe_alloc_buf_from_group 이 size 매칭 buffer 를 찾을 때.
	 *   값 범위: 비어있을 수도 있음 (assert 로 destroy 시 비어있어야 함).
	 *   동기화: 같은 reactor 내 단일 스레드 사용 가정 — 별도 lock 없음. */
};

/* [한국어] 파이프의 본체.
 *  - buf: 외부에서 주입된 N 바이트 버퍼 (group 사용 시 NULL 가능).
 *  - 원형 버퍼 의미: write 포인터는 producer 가 진행, read 는 consumer 가 진행.
 *    write == read 일 때는 "비어있다(full=false)" 또는 "꽉 찼다(full=true)" 두 의미 있음.
 *  - SPSC: producer 1, consumer 1 가정. atomic 없이도 데이터 일관성 보장. */
struct spdk_pipe {
	uint8_t	*buf;
	/* [한국어] 데이터 영역 시작 주소. group 사용 시 빈 상태에서는 NULL 가능.
	 *   설정자: spdk_pipe_create / pipe_alloc_buf_from_group / group_remove.
	 *   읽는 자: writer_get_buffer / reader_get_buffer 등 모든 데이터 접근.
	 *   값 범위: NULL (group 사용 + 비어있음) 또는 pipe->sz 바이트의 유효 메모리.
	 *   동기화: SPSC 단일 스레드 가정. */
	uint32_t sz;
	/* [한국어] 버퍼 총 크기(바이트). 생성 시 고정, 이후 불변.
	 *   설정자: spdk_pipe_create.
	 *   읽는 자: 모든 wrap 계산 (write/read 의 modulo 기준).
	 *   값 범위: 호출자 지정 양수. */

	uint32_t write;
	/* [한국어] 다음 producer 쓰기 시작 오프셋 (modulo sz).
	 *   설정자: writer_advance.
	 *   읽는 자: writer_get_buffer (자유 공간 계산), reader_get_buffer (가용 데이터 계산).
	 *   값 범위: [0, sz). sz 에 도달하면 0 으로 wrap. */
	uint32_t read;
	/* [한국어] 다음 consumer 읽기 시작 오프셋 (modulo sz).
	 *   설정자: reader_advance.
	 *   읽는 자: writer_get_buffer (남은 free 공간 계산), reader_get_buffer (가용 데이터 계산).
	 *   값 범위: [0, sz). */
	bool full;
	/* [한국어] read==write 일 때 "꽉 참" vs "비어있음" 을 구별하기 위한 플래그.
	 *   설정자: writer_advance 가 read 와 동일해질 때 true, reader_advance 직후 false.
	 *   읽는 자: 가용 byte 계산 분기.
	 *   값 범위: true(가득) / false(공간 있음 또는 비어있음). */

	struct spdk_pipe_group *group;
	/* [한국어] 이 pipe 가 buffer 를 공유받는 group 포인터. NULL 이면 group 미사용.
	 *   설정자: spdk_pipe_group_add 가 설정, group_remove 가 해제.
	 *   읽는 자: writer_get_buffer (lazy alloc), reader_advance (lazy free).
	 *   값 범위: NULL 또는 valid group 포인터. */
};

/*
 * [한국어]
 * spdk_pipe_create - 외부 buffer 를 받아 spdk_pipe 객체를 만든다.
 *
 * @buf: 호출자가 소유한 N 바이트 메모리. pipe 는 이를 빌리기만 하며 free 하지 않는다.
 * @sz:  buf 의 크기.
 * @return: 성공 시 pipe 포인터, 실패(메모리 부족) 시 NULL.
 *
 * 호출 후 group 이 없으면 buf 가 곧바로 사용된다. group 에 add 되면 해당
 * buffer 를 group 풀로 반환할 수도 있다.
 */
struct spdk_pipe *
spdk_pipe_create(void *buf, uint32_t sz)
{
	struct spdk_pipe *pipe;

	/* [한국어] pipe 메타 객체 할당 — calloc 으로 모든 포인터/카운터 0 초기화. */
	pipe = calloc(1, sizeof(*pipe));
	if (pipe == NULL) {
		return NULL;
	}

	/* [한국어] 외부 buffer 와 그 크기를 그대로 흡수. */
	pipe->buf = buf;
	pipe->sz = sz;

	return pipe;
}

/*
 * [한국어]
 * spdk_pipe_destroy - pipe 를 해제하고 사용 중이던 buffer 를 반환.
 *
 * @pipe:   해제 대상. NULL 이면 NULL 반환.
 * @return: 호출자에게 돌려줄 원본 buffer 포인터 (호출자가 free 또는 재사용).
 *
 * 만약 group 에 속해 있었다면 먼저 group 에서 제거하면서 buffer 도 회수한다.
 */
void *
spdk_pipe_destroy(struct spdk_pipe *pipe)
{
	void *buf;

	if (pipe == NULL) {
		return NULL;
	}

	/* [한국어] group 소속이면 먼저 group 에서 분리 — buffer 의 ownership 정리. */
	if (pipe->group) {
		spdk_pipe_group_remove(pipe->group, pipe);
	}

	/* [한국어] 사용 중이던 buffer 포인터를 백업한 뒤 메타 객체 해제. */
	buf = pipe->buf;
	free(pipe);
	return buf;
}

/*
 * [한국어]
 * pipe_alloc_buf_from_group (static) - group 풀에서 사이즈가 일치하는 buffer 를 빌려온다.
 *
 * @pipe: buffer 가 필요한 pipe (group 가 설정되어 있어야 함).
 *
 * 동작: free-list 를 처음부터 순회하며 sz 가 같은 buffer 를 찾아 SLIST_REMOVE.
 * 그 buffer 의 메모리 시작 주소를 pipe->buf 로 사용. 풀에는 항상 일치하는 항목이
 * 있어야 한다는 가정(같은 reactor 의 group 은 동일 size buffer 만 보유)이며,
 * 그렇지 않으면 assert(false) 로 즉시 실패한다.
 *
 * 동시성: SPSC + 단일 스레드 group 사용 가정 → lock-free.
 */
static void
pipe_alloc_buf_from_group(struct spdk_pipe *pipe)
{
	struct spdk_pipe_buf *buf;
	struct spdk_pipe_group *group;

	/* [한국어] 사전조건: group 이 반드시 설정되어 있어야 함. */
	assert(pipe->group != NULL);
	group = pipe->group;

	/* We have to pick a buffer that's the correct size. It's almost always
	 * the first one. */
	/* [한국어] free-list 의 head 부터 size 매칭을 검사 — 일반적으로 head 가 답이다. */
	buf = SLIST_FIRST(&group->bufs);
	while (buf != NULL) {
		if (buf->sz == pipe->sz) {
			/* TODO: Could track the previous and do an SLIST_REMOVE_AFTER */
			/* [한국어] 일치 → 풀에서 떼어 pipe 가 흡수. SLIST_REMOVE 는 O(n) 이지만
			 * 풀 크기가 보통 작아 문제되지 않는다. */
			SLIST_REMOVE(&pipe->group->bufs, buf, spdk_pipe_buf, link);
			pipe->buf = (void *)buf;
			return;
		}
		buf = SLIST_NEXT(buf, link);
	}
	/* Should never get here. */
	/* [한국어] 도달하면 풀 관리에 모순 발생 — 즉시 단언 실패. */
	assert(false);
}

/*
 * [한국어]
 * spdk_pipe_writer_get_buffer - producer 가 쓸 수 있는 자유 영역을 iovs 두 조각으로 노출.
 *
 * @pipe:         대상 pipe.
 * @requested_sz: 요청하는 최대 자유 공간(바이트).
 * @iovs:         out. 두 조각 (wrap 시 둘 다 사용, 아니면 [1] 은 0).
 * @return:       총 노출된 바이트 수 (iovs[0].iov_len + iovs[1].iov_len).
 *
 * SPSC 의미: write 만 producer 가 갱신하므로, write 를 읽을 때 이전 갱신은
 * 이미 visible. read 는 consumer 가 갱신하지만 요청된 free 공간 계산에서
 * "보수적으로 작은 값" 이 나오는 것은 안전 (실제로는 더 많은 공간이 있을 수 있음).
 * 따라서 atomic load 없이도 정확성 유지 (SPDK 의 일반 사용 패턴 기준).
 */
int
spdk_pipe_writer_get_buffer(struct spdk_pipe *pipe, uint32_t requested_sz, struct iovec *iovs)
{
	uint32_t sz;
	uint32_t read;
	uint32_t write;

	/* [한국어] 현재 read/write 스냅샷 — 함수 내 일관된 분석을 위해 로컬 복사. */
	read = pipe->read;
	write = pipe->write;

	/* [한국어] 가득 찼거나 0 요청 → 빈 iov 로 즉시 반환. */
	if (pipe->full || requested_sz == 0) {
		iovs[0].iov_base = NULL;
		iovs[0].iov_len = 0;
		return 0;
	}

	/* [한국어] group 사용 중이면서 buffer 가 떨어진 상태라면 — 풀에서 lazy alloc. */
	if (pipe->buf == NULL) {
		pipe_alloc_buf_from_group(pipe);
	}

	if (read <= write) {
		/* [한국어] write 가 read 보다 같거나 뒤에 있는 케이스:
		 *   자유 영역 = [write, sz) ∪ [0, read).
		 *   첫 조각: [write, sz). 둘째 조각: [0, read). */
		sz = spdk_min(requested_sz, pipe->sz - write);

		iovs[0].iov_base = pipe->buf + write;
		iovs[0].iov_len = sz;

		requested_sz -= sz;

		if (requested_sz > 0) {
			/* [한국어] 첫 조각으로 부족한 만큼 wrap 해 두 번째 조각 노출. */
			sz = spdk_min(requested_sz, read);

			iovs[1].iov_base = (sz == 0) ? NULL : pipe->buf;
			iovs[1].iov_len = sz;
		} else {
			/* [한국어] 첫 조각만으로 충분 — 두 번째는 빈 슬롯. */
			iovs[1].iov_base = NULL;
			iovs[1].iov_len = 0;
		}
	} else {
		/* [한국어] write 가 read 보다 앞쪽에 있는 케이스 (이미 wrap 한 상황):
		 *   자유 영역 = [write, read). 단일 조각. */
		sz = spdk_min(requested_sz, read - write);

		iovs[0].iov_base = pipe->buf + write;
		iovs[0].iov_len = sz;
		iovs[1].iov_base = NULL;
		iovs[1].iov_len = 0;
	}

	return iovs[0].iov_len + iovs[1].iov_len;
}

/*
 * [한국어]
 * spdk_pipe_writer_advance - producer 가 실제 채운 만큼 write 포인터 진행.
 *
 * @requested_sz: 직전 get_buffer 에서 노출 받았던 영역 중 실제로 채운 바이트.
 * @return:       0 성공, -EINVAL 자유 공간 초과 또는 full 상태.
 *
 * read/write 동치점 도달 시 full 비트 set. write 는 sz 도달 시 0 으로 wrap.
 */
int
spdk_pipe_writer_advance(struct spdk_pipe *pipe, uint32_t requested_sz)
{
	uint32_t sz;
	uint32_t read;
	uint32_t write;

	read = pipe->read;
	write = pipe->write;

	/* [한국어] 전체 buffer 보다 큰 진행 또는 이미 full 인 상태에서의 진행은 거절. */
	if (requested_sz > pipe->sz || pipe->full) {
		return -EINVAL;
	}

	if (read <= write) {
		/* [한국어] 자유 공간 = (sz - write) + read. 그것보다 크면 거절. */
		if (requested_sz > (pipe->sz - write) + read) {
			return -EINVAL;
		}

		/* [한국어] 우선 첫 segment(=끝까지) 만큼 진행. */
		sz = spdk_min(requested_sz, pipe->sz - write);

		write += sz;
		if (write == pipe->sz) {
			/* [한국어] 끝에 도달 → 0 으로 wrap. */
			write = 0;
		}
		requested_sz -= sz;

		/* [한국어] 남았으면 wrap 후 그 만큼만 추가 진행. */
		if (requested_sz > 0) {
			write = requested_sz;
		}
	} else {
		/* [한국어] 이미 wrap 된 상태 — 자유 공간 = read - write. */
		if (requested_sz > (read - write)) {
			return -EINVAL;
		}

		write += requested_sz;
	}

	/* [한국어] read 와 같아지면 가득 찬 상태 — full=true 로 표시 (조회 분기에 사용). */
	if (read == write) {
		pipe->full = true;
	}
	pipe->write = write;

	return 0;
}

/*
 * [한국어]
 * spdk_pipe_reader_bytes_available - consumer 가 즉시 읽을 수 있는 바이트 수 반환.
 *
 * 분기:
 *   read == write && !full → 0 (빈 상태)
 *   read <  write          → write - read
 *   그 외(=wrap 또는 full) → (sz - read) + write
 */
uint32_t
spdk_pipe_reader_bytes_available(struct spdk_pipe *pipe)
{
	uint32_t read;
	uint32_t write;

	/* [한국어] 스냅샷 로딩 (atomic 비사용 — SPSC + 단일 스레드 가정). */
	read = pipe->read;
	write = pipe->write;

	if (read == write && !pipe->full) {
		/* [한국어] 비어있음. */
		return 0;
	} else if (read < write) {
		/* [한국어] 단순 케이스 — read 부터 write 직전까지. */
		return write - read;
	} else {
		/* [한국어] wrap 또는 가득참 — 끝까지 + 0~write. */
		return (pipe->sz - read) + write;
	}
}

/*
 * [한국어]
 * spdk_pipe_reader_get_buffer - consumer 측 가용 데이터 영역을 iovs 두 조각으로 노출.
 *
 * writer_get_buffer 의 reader 버전. 비어있거나 0 요청이면 빈 iov.
 * read < write 면 단일 조각, 그 외엔 [read, sz) + [0, write) 두 조각.
 */
int
spdk_pipe_reader_get_buffer(struct spdk_pipe *pipe, uint32_t requested_sz, struct iovec *iovs)
{
	uint32_t sz;
	uint32_t read;
	uint32_t write;

	read = pipe->read;
	write = pipe->write;

	if ((read == write && !pipe->full) || requested_sz == 0) {
		/* [한국어] 데이터 없음 또는 0 요청 → 빈 iovs. */
		iovs[0].iov_base = NULL;
		iovs[0].iov_len = 0;
		iovs[1].iov_base = NULL;
		iovs[1].iov_len = 0;
	} else if (read < write) {
		/* [한국어] read..write 한 조각 케이스 — wrap 없음. */
		sz = spdk_min(requested_sz, write - read);

		iovs[0].iov_base = pipe->buf + read;
		iovs[0].iov_len = sz;
		iovs[1].iov_base = NULL;
		iovs[1].iov_len = 0;
	} else {
		/* [한국어] wrap (또는 full) — [read, sz) + [0, write). */
		sz = spdk_min(requested_sz, pipe->sz - read);

		iovs[0].iov_base = pipe->buf + read;
		iovs[0].iov_len = sz;

		requested_sz -= sz;

		if (requested_sz > 0) {
			/* [한국어] 첫 조각으로 부족 → 두 번째 조각 노출 (wrap 영역). */
			sz = spdk_min(requested_sz, write);
			iovs[1].iov_base = (sz == 0) ? NULL : pipe->buf;
			iovs[1].iov_len = sz;
		} else {
			iovs[1].iov_base = NULL;
			iovs[1].iov_len = 0;
		}
	}

	return iovs[0].iov_len + iovs[1].iov_len;
}

/*
 * [한국어]
 * spdk_pipe_reader_advance - consumer 가 실제 소비한 만큼 read 포인터 진행.
 *
 * @requested_sz: 직전 get_buffer 에서 노출 받았던 영역 중 실제로 처리한 바이트.
 * @return:       0 성공, -EINVAL 가용 데이터 초과.
 *
 * 읽기 후 비어있게 되면 read/write 를 0 으로 리셋하고, group 사용 중이면
 * buffer 를 풀로 반환 (다른 pipe 가 재사용 가능하도록 lazy free).
 */
int
spdk_pipe_reader_advance(struct spdk_pipe *pipe, uint32_t requested_sz)
{
	uint32_t sz;
	uint32_t read;
	uint32_t write;

	read = pipe->read;
	write = pipe->write;

	/* [한국어] 0 요청 → no-op (full 비트 토글 방지). */
	if (requested_sz == 0) {
		return 0;
	}

	if (read < write) {
		/* [한국어] 단순 케이스 — write - read 만큼만 진행 가능. */
		if (requested_sz > (write - read)) {
			return -EINVAL;
		}

		read += requested_sz;
	} else {
		/* [한국어] wrap 또는 full — [read, sz) 까지 우선 진행. */
		sz = spdk_min(requested_sz, pipe->sz - read);

		read += sz;
		if (read == pipe->sz) {
			/* [한국어] 끝 도달 → 0 으로 wrap. */
			read = 0;
		}
		requested_sz -= sz;

		if (requested_sz > 0) {
			/* [한국어] wrap 후 추가로 진행할 양은 write 까지여야 함. */
			if (requested_sz > write) {
				return -EINVAL;
			}

			read = requested_sz;
		}
	}

	/* We know we advanced at least one byte, so the pipe isn't full. */
	/* [한국어] 한 바이트라도 소비했으니 더 이상 가득 차지 않음. */
	pipe->full = false;

	if (read == write) {
		/* The pipe is empty. To re-use the same memory more frequently, jump
		 * both pointers back to the beginning of the pipe. */
		/* [한국어] 비어있는 상태 — 양쪽 포인터를 0 으로 리셋해 캐시 친화적 재사용 유도. */
		read = 0;
		pipe->write = 0;

		/* Additionally, release the buffer to the shared pool */
		/* [한국어] group 풀이 있다면 buffer 를 즉시 풀로 반환 (lazy free).
		 *  buffer 의 시작 부분에 spdk_pipe_buf 메타데이터를 덮어 써서 free-list 노드로 변환. */
		if (pipe->group) {
			struct spdk_pipe_buf *buf = (struct spdk_pipe_buf *)pipe->buf;
			buf->sz = pipe->sz;
			SLIST_INSERT_HEAD(&pipe->group->bufs, buf, link);
			pipe->buf = NULL;
		}
	}

	pipe->read = read;

	return 0;
}

/*
 * [한국어]
 * spdk_pipe_group_create - 빈 buffer 풀 group 생성.
 *
 * @return: 성공 시 group 포인터, 실패 시 NULL.
 */
struct spdk_pipe_group *
spdk_pipe_group_create(void)
{
	struct spdk_pipe_group *group;

	/* [한국어] 메타 객체 0 초기화 할당. */
	group = calloc(1, sizeof(*group));
	if (!group) {
		return NULL;
	}

	/* [한국어] free-list 헤드 초기화 — 빈 리스트로 출발. */
	SLIST_INIT(&group->bufs);

	return group;
}

/*
 * [한국어]
 * spdk_pipe_group_destroy - group 해제. 풀이 비어있어야 안전(누수 검출).
 */
void
spdk_pipe_group_destroy(struct spdk_pipe_group *group)
{
	/* [한국어] free-list 가 남아있으면 어딘가에 buffer 를 회수하지 않은 것 — 진단 출력 후 단언. */
	if (!SLIST_EMPTY(&group->bufs)) {
		SPDK_ERRLOG("Destroying a pipe group that still has buffers!\n");
		assert(false);
	}

	free(group);
}

/*
 * [한국어]
 * spdk_pipe_group_add - pipe 를 group 에 등록. 비어있는 buffer 면 곧바로 풀로 회수.
 *
 * @group: 대상 풀.
 * @pipe:  추가할 파이프 (이미 group 소속이 아니어야 함).
 * @return: 0 (현재 구현에선 항상 성공).
 */
int
spdk_pipe_group_add(struct spdk_pipe_group *group, struct spdk_pipe *pipe)
{
	struct spdk_pipe_buf *buf;

	/* [한국어] 이중 등록 방지 — 사전조건. */
	assert(pipe->group == NULL);

	/* [한국어] group 포인터 설정 — 이후 lazy alloc/free 활성. */
	pipe->group = group;
	if (pipe->read != pipe->write || pipe->full) {
		/* Pipe currently has valid data, so keep the buffer attached
		 * to the pipe for now.  We can move it to the group's SLIST
		 * later when it gets emptied.
		 */
		/* [한국어] 데이터가 남아있으면 buffer 를 즉시 회수하지 않음.
		 *  비어진 시점(reader_advance) 에 자동으로 풀로 반환됨. */
		return 0;
	}

	/* [한국어] 비어있는 상태 → buffer 를 즉시 free-list 로 이동.
	 *  buffer 의 첫 sizeof(spdk_pipe_buf) 바이트를 메타데이터로 덮어씀. */
	buf = (struct spdk_pipe_buf *)pipe->buf;
	buf->sz = pipe->sz;
	SLIST_INSERT_HEAD(&group->bufs, buf, link);
	pipe->buf = NULL;
	return 0;
}

/*
 * [한국어]
 * spdk_pipe_group_remove - pipe 를 group 에서 분리. buffer 가 없으면 풀에서 빌려서 부착.
 *
 * 주의: pipe 는 group 분리 후에도 자체 buffer 를 가져야 사용 가능하므로,
 * 풀에서 size 가 같은 buffer 를 빌려 부착해 두고 분리한다.
 */
int
spdk_pipe_group_remove(struct spdk_pipe_group *group, struct spdk_pipe *pipe)
{
	/* [한국어] 사전조건: 정말 이 group 에 속해 있던 pipe 인가. */
	assert(pipe->group == group);

	if (pipe->buf == NULL) {
		/* Associate a buffer with the pipe before returning. */
		/* [한국어] buffer 가 없는 상태 → 풀에서 가져와 부착. */
		pipe_alloc_buf_from_group(pipe);
		assert(pipe->buf != NULL);
	}

	/* [한국어] 분리 — 이후 pipe 는 group 의 lazy alloc/free 미사용. */
	pipe->group = NULL;
	return 0;
}
