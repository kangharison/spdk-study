/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation. All rights reserved.
 */

/** \file
 * A pipe that is intended for buffering data between a source, such as
 * a socket, and a sink, such as a parser, or vice versa. Any time data
 * is received in units that differ from the the units it is consumed
 * in may benefit from using a pipe.
 *
 * The pipe is not thread safe. Only a single thread can act as both
 * the producer (called the writer) and the consumer (called the reader).
 */

/*
 * [한국어 설명] 단일 스레드 링 버퍼 파이프 공개 API (pipe.h)
 *
 * === 파일의 역할 ===
 * **단일 스레드 내 producer-consumer 버퍼링**을 위한 가벼운 링 버퍼(circular buffer)를 제공한다.
 * 핵심 시나리오는 "데이터가 들어오는 단위(예: 소켓 read 청크)"와 "소비되는 단위(예: 메시지/PDU
 * 파서가 한 번에 요구하는 양)"가 다를 때 그 사이의 임피던스 미스매치를 흡수하는 것.
 *
 * 두 가지 자료구조가 있다:
 *   1. `struct spdk_pipe`: 사용자가 제공한 메모리 버퍼를 감싸는 단일 파이프
 *   2. `struct spdk_pipe_group`: 여러 pipe가 공유하는 데이터 버퍼 풀 — 빈 상태가 된 pipe가 자신의
 *      버퍼를 풀에 반납하면 다른 pipe가 새로 필요할 때 거기서 빌려 씀. **스택 LRU** 방식이라 동일
 *      버퍼가 자주 재사용되어 캐시 친화적.
 *
 * 구현은 `lib/util/pipe.c`. 동시성: **non-thread-safe**. 한 pipe는 단일 스레드만 reader/writer
 * 역할을 모두 수행해야 한다 (cross-thread 시 race).
 *
 * 주요 사용처:
 *   - `lib/sock/`: 소켓 수신 데이터 누적 → PDU 파서가 부분 읽기
 *   - `lib/iscsi/`, `lib/nvmf/tcp/`: TCP에서 받은 바이트 → SCSI/NVMe-oF 명령 PDU 어셈블링
 *
 * === 전체 아키텍처에서의 위치 ===
 * **유틸리티 계층(util)**. 사용은 거의 항상 SPDK thread 내부의 단일 producer-consumer 컨텍스트.
 * I/O hot-path에서 호출되며, write/read 모두 zero-copy 친화적인 iovec 인터페이스를 사용해
 * 링 wrap 시 두 영역으로 분리된 메모리에 그대로 노출.
 *
 * 호출 체인(전형적 — 소켓 수신):
 *   epoll/poll → recv 가능 알림
 *     → spdk_pipe_writer_get_buffer(pipe, sz, iovs)  // 빈 영역 iovec 받기
 *     → recvmsg(sock, iovs)                          // 커널이 iovec에 직접 복사 (zero-copy)
 *     → spdk_pipe_writer_advance(pipe, recv_bytes)   // 실제 받은 바이트만큼 진행
 *   파서 루프
 *     → spdk_pipe_reader_bytes_available(pipe)
 *     → spdk_pipe_reader_get_buffer(pipe, want, iovs)
 *     → 파싱
 *     → spdk_pipe_reader_advance(pipe, consumed_bytes)
 *
 * === 타 모듈과의 연결 ===
 * - `spdk/stdinc.h`: 기본 타입 + `struct iovec` (sys/uio.h 경유)
 * - `lib/sock/`: 소켓 수신 누적 버퍼로 사용
 * - `lib/iscsi/conn.c`, `lib/nvmf/tcp.c`: PDU 어셈블러
 *
 * 데이터 흐름:
 *   [네트워크/장치]
 *     → spdk_pipe_writer_get_buffer (iovec 노출, 보통 1~2 entry — 링 wrap 시 2)
 *     → recvmsg/read 등으로 iovec에 직접 채움
 *     → spdk_pipe_writer_advance(N)로 write head 전진
 *     → spdk_pipe_reader_bytes_available로 사용 가능량 확인
 *     → spdk_pipe_reader_get_buffer (iovec 노출)
 *     → 파서가 iovec 내용 소비
 *     → spdk_pipe_reader_advance(M)로 read head 전진
 *
 * === 주요 함수/구조체 요약 ===
 * - `struct spdk_pipe`: 불투명. 내부에 user buffer 포인터, size, write_idx, read_idx, full 플래그.
 * - `struct spdk_pipe_group`: 빈 pipe들의 데이터 버퍼를 모아두는 스택 — 재사용 풀.
 * - `spdk_pipe_create / destroy`: 생애주기. destroy는 버퍼를 free하지 않고 호출자에게 반환.
 * - **writer 측**:
 *   · `spdk_pipe_writer_get_buffer(sz, iovs)` — 쓰기 영역 노출 (linked iovec 2개)
 *   · `spdk_pipe_writer_advance(count)` — write head 진행 (커밋)
 * - **reader 측**:
 *   · `spdk_pipe_reader_bytes_available()` — 읽을 수 있는 바이트 수
 *   · `spdk_pipe_reader_get_buffer(sz, iovs)` — 읽기 영역 노출
 *   · `spdk_pipe_reader_advance(count)` — read head 진행 (소비 확정)
 * - **group**:
 *   · `spdk_pipe_group_create / destroy / add / remove`
 *
 * 경계/주의:
 *   - get_buffer 두 번 호출하면 같은 영역 반환 (advance 호출 전까지)
 *   - writer는 reader 위치를 추월할 수 없음 (full 검사)
 *   - group은 빈 pipe의 버퍼를 공유 — pipe가 다시 데이터를 원할 때 다른 버퍼를 받을 수 있음
 *     → destroy 반환값이 "원래 create 시 전달한 버퍼와 다를 수 있다"는 점 주의
 *   - non-thread-safe: 단일 스레드 producer + consumer로 사용
 */

#ifndef SPDK_PIPE_H
#define SPDK_PIPE_H

/* [한국어] 표준 타입 + struct iovec(sys/uio.h)을 위해 포함. */
#include "spdk/stdinc.h"

#ifdef __cplusplus
/* [한국어] C++ 환경에서 C 링키지 보장. */
extern "C" {
#endif

/* [한국어] 단일 파이프의 불투명 전방 선언.
 *   - 내부는 lib/util/pipe.c에서만 정의 (data 포인터, size, write/read 인덱스, group 링크 등).
 *   - non-thread-safe. */
struct spdk_pipe;

/* [한국어] 파이프 그룹(빈 버퍼 공유 풀)의 불투명 전방 선언.
 *   - 같은 그룹에 속한 pipe들이 사용하지 않는 동안 데이터 버퍼를 풀에 모아두고, 누군가 데이터를
 *     원할 때 풀에서 꺼내 빌려준다.
 *   - 스택 자료구조 — 가장 최근 반납된 버퍼가 다음 할당 시 재사용 (LRU 캐시 친화적). */
struct spdk_pipe_group;

/**
 * Construct a pipe around the given memory buffer. The pipe treats the memory
 * buffer as a circular ring of bytes.
 *
 * \param buf The data buffer that backs this pipe.
 * \param sz The size of the data buffer.
 *
 * \return spdk_pipe. The new pipe.
 */
/*
 * [한국어]
 * spdk_pipe_create - 호출자 제공 메모리 버퍼를 감싸 새 pipe 인스턴스 생성.
 *
 * @buf: pipe의 데이터 영역으로 사용할 메모리. 호출자가 미리 할당해야 하며, pipe가 살아있는 동안
 *       이 버퍼를 다른 용도로 사용/해제하면 안 됨 (소유권은 호출자에게 유지되지만 pipe가 참조).
 *       일반적으로 socket/PDU 누적용 임시 버퍼 — 4KB ~ 64KB 등.
 * @sz:  buf의 바이트 크기. 링의 capacity.
 * @return: 새 spdk_pipe 포인터. 실패 시 NULL (보통 메타데이터 malloc 실패).
 *
 * 왜 필요한가: pipe 자료구조 자체는 메타데이터(인덱스 등)만 가지고 데이터는 호출자 버퍼를 사용해
 *              호출자가 메모리 정책을 완전히 통제할 수 있게 한다 (DPDK hugepage, NUMA-local 등).
 * 동작:
 *   1. spdk_pipe 메타 구조체 malloc
 *   2. data=buf, size=sz, write_idx=0, read_idx=0, full=false 초기화
 * 실행 컨텍스트: 초기화 경로.
 * 호출 체인:
 *   sock/iscsi/nvmf 연결 init → [spdk_pipe_create]
 */
struct spdk_pipe *spdk_pipe_create(void *buf, uint32_t sz);

/**
 * Destroys the pipe. This does not release the buffer, but does
 * make it safe for the user to release the buffer.
 *
 * \param pipe The pipe to operate on.
 * \return Pipe buffer associated with the pipe when destroyed.  The
 *         caller should free this buffer.  It may not be the same
 *         buffer that was passed to spdk_pipe_create.
 */
/*
 * [한국어]
 * spdk_pipe_destroy - pipe 메타 구조체 해제. 데이터 버퍼는 free하지 않고 반환.
 *
 * @pipe: 해제할 pipe.
 * @return: 이 pipe가 마지막에 사용하던 데이터 버퍼 포인터. 호출자가 직접 free 책임.
 *          **주의**: pipe가 group에 속해 있었다면 create 시 전달한 버퍼와 **다를 수 있음**
 *          (그룹 풀에서 다른 버퍼와 swap된 후 destroy되었을 가능성).
 *
 * 왜 필요한가: pipe 인스턴스는 종료하되 데이터 버퍼는 호출자가 회수해야 함 (호출자가 처음에 할당했고
 *              메모리 정책을 통제하므로). group 사용 시 버퍼 교체 가능성을 명시적으로 알려준다.
 * 동작:
 *   1. group에 속해 있으면 group_remove와 동등한 분리 처리
 *   2. 현재 data 포인터 추출
 *   3. spdk_pipe 구조체 free
 *   4. 데이터 포인터 반환
 * 실행 컨텍스트: 종료 경로.
 * 호출 체인:
 *   연결 종료 → [spdk_pipe_destroy] → 호출자가 free(buf)
 */
void *spdk_pipe_destroy(struct spdk_pipe *pipe);

/**
 * Acquire memory from the pipe for writing.
 *
 * This function will acquire up to sz bytes from the pipe to be used for
 * writing. It may return fewer total bytes.
 *
 * The memory is only marked as consumed upon a call to spdk_pipe_writer_advance().
 * Multiple calls to this function without calling advance return the same region
 * of memory.
 *
 * \param pipe The pipe to operate on.
 * \param sz The size requested.
 * \param iovs A two element iovec array that will be populated with the requested memory.
 *
 * \return The total bytes obtained. May be 0.
 */
/*
 * [한국어]
 * spdk_pipe_writer_get_buffer - 쓰기 가능한 메모리 영역을 iovec으로 노출 (zero-copy).
 *
 * @pipe: 대상 pipe.
 * @sz:   요청 바이트 수 (희망 최대).
 * @iovs: [in/out] **반드시 2-원소 배열**. 함수가 iovs[0]/iovs[1]에 메모리 영역을 채움.
 *        - 단일 연속 영역(링이 wrap되지 않음): iovs[0]만 valid (iov_len > 0), iovs[1].iov_len == 0
 *        - wrap된 두 영역: iovs[0] = 끝까지, iovs[1] = 처음부터 — 호출자는 두 영역에 걸쳐 써야 함
 * @return: 두 iovec의 iov_len 합 (실제 확보된 총 바이트 수). 0이면 가득 찬 상태 — backpressure 필요.
 *          음수면 에러.
 *
 * 왜 필요한가: recvmsg/readv 같은 scatter-gather I/O와 직접 호환되어 zero-copy 데이터 수신 가능.
 *              writer는 sz만큼 받아 write 후 advance로 커밋하는 패턴.
 * 중요한 시맨틱:
 *   - 같은 영역을 advance 호출 전까지 반복 호출 가능 (idempotent)
 *   - 실제로 쓴 바이트 수는 다음 advance에서 결정 (덜 써도 됨)
 *   - reader 위치 직전까지만 노출 (overrun 방지)
 * 동작:
 *   1. 현재 read_idx와 write_idx로부터 빈 공간 계산
 *   2. min(sz, available)만큼 노출
 *   3. 링 wrap이면 iovs[0]/iovs[1]로 분할
 * 실행 컨텍스트: 단일 스레드 (writer 역할).
 * 호출 체인:
 *   sock recv 콜백 → [spdk_pipe_writer_get_buffer] → recvmsg(iovs)
 */
int spdk_pipe_writer_get_buffer(struct spdk_pipe *pipe, uint32_t sz, struct iovec *iovs);

/**
 * Advance the write pointer by the given number of bytes
 *
 * The user can obtain memory from the pipe using spdk_pipe_writer_get_buffer(),
 * but only calling this function marks it as consumed. The user is not required
 * to advance the same number of bytes as was obtained from spdk_pipe_writer_get_buffer().
 * However, upon calling this function, the previous memory region is considered
 * invalid and the user must call spdk_pipe_writer_get_buffer() again to obtain
 * additional memory.
 *
 * The user cannot advance past the current read location.
 *
 * \param pipe The pipe to operate on.
 * \param count The number of bytes to advance.
 *
 * \return On error, a negated errno. On success, 0.
 */
/*
 * [한국어]
 * spdk_pipe_writer_advance - write head를 count 바이트 전진 (커밋 액션).
 *
 * @pipe:  대상 pipe.
 * @count: 실제로 쓴 바이트 수. get_buffer가 반환한 양보다 작아도 됨 (덜 쓴 케이스 허용).
 *         단, reader 위치를 추월하면 안 됨 — 추월 시도 시 -EINVAL 등 에러 반환.
 * @return: 0 = 성공. 음수 errno = 실패.
 *
 * 왜 필요한가: get_buffer로 받은 영역 중 **실제로 채운 바이트만** 데이터로 확정. 예: recvmsg가
 *              요청보다 적게 받았을 때 그 양만큼만 advance.
 * 중요한 시맨틱:
 *   - advance 호출 후에는 직전에 받은 메모리 영역이 invalidate — 추가 쓰기 전에 get_buffer 재호출 필수
 *   - reader가 바라보는 데이터의 **끝 위치**가 이 호출로 갱신됨 (consumer가 볼 수 있게 됨)
 * 동작: write_idx = (write_idx + count) % size, full 플래그 갱신.
 * 실행 컨텍스트: 단일 스레드 (writer).
 * 호출 체인:
 *   sock recv 후 → [spdk_pipe_writer_advance(actual_recv)]
 */
int spdk_pipe_writer_advance(struct spdk_pipe *pipe, uint32_t count);

/**
 * Get the number of bytes available to read from the pipe.
 *
 * \param pipe The pipe to operate on.
 *
 * \return The number of bytes available for reading.
 */
/*
 * [한국어]
 * spdk_pipe_reader_bytes_available - 읽을 수 있는 데이터 바이트 수 반환.
 *
 * @pipe: 대상 pipe.
 * @return: 현재 reader가 읽을 수 있는 바이트 수 (= write_idx - read_idx, full 시 size).
 *
 * 왜 필요한가: 파서가 "PDU 헤더 N바이트가 도착했나?" 검사를 위해 자주 호출. 빈 상태/부분 도착 등을
 *              조기에 판단해 unnecessary get_buffer 호출 회피.
 * 동작: 인덱스 차이 계산 — O(1).
 * 실행 컨텍스트: reader 측 호출.
 * 호출 체인:
 *   PDU 파서 → [spdk_pipe_reader_bytes_available]
 */
uint32_t spdk_pipe_reader_bytes_available(struct spdk_pipe *pipe);

/**
 * Obtain previously written memory from the pipe for reading.
 *
 * This call populates the two element iovec provided with a region
 * of memory containing the next available data in the pipe. The size
 * will be up to sz bytes, but may be less.
 *
 * Calling this function does not mark the memory as consumed. Calling this function
 * twice without a call to spdk_pipe_reader_advance in between will return the same
 * region of memory.
 *
 * \param pipe The pipe to operate on.
 * \param sz The size requested.
 * \param iovs A two element iovec array that will be populated with the requested memory.
 *
 * \return On error, a negated errno. On success, the total number of bytes available.
 */
/*
 * [한국어]
 * spdk_pipe_reader_get_buffer - 읽을 수 있는 영역을 iovec으로 노출 (zero-copy).
 *
 * @pipe: 대상 pipe.
 * @sz:   읽고자 하는 최대 바이트 수.
 * @iovs: [in/out] 2-원소 iovec 배열. 링이 wrap된 데이터일 경우 두 영역으로 분리.
 * @return: 두 iovec의 iov_len 합. 음수면 에러.
 *
 * 왜 필요한가: 파서가 데이터 영역을 직접 읽거나, sendmsg/writev에 그대로 전달 가능 (zero-copy 송신).
 * 중요한 시맨틱:
 *   - advance 전까지 같은 영역을 반복해서 받을 수 있음 (rewind 가능)
 *   - 즉, "헤더만 일단 보고 PDU 길이 확인 → 전체 PDU가 도착했으면 한꺼번에 advance" 패턴 가능
 * 동작: read_idx로부터 사용 가능한 데이터를 sz까지 노출. wrap 시 분할.
 * 실행 컨텍스트: 단일 스레드 (reader).
 * 호출 체인:
 *   PDU 파서 → [spdk_pipe_reader_get_buffer] → memcpy / sendmsg
 */
int spdk_pipe_reader_get_buffer(struct spdk_pipe *pipe, uint32_t sz, struct iovec *iovs);

/**
 * Mark memory as read, making it available for writing. The user is not required
 * to advance the same number of byte as was obtained by a previous call to
 * spdk_pipe_reader_get_buffer().
 *
 * \param pipe The pipe to operate on.
 * \param count The number of bytes to advance.
 *
 * \return On error, a negated errno. On success, 0.
 */
/*
 * [한국어]
 * spdk_pipe_reader_advance - read head를 count 바이트 전진 (소비 확정).
 *
 * @pipe:  대상 pipe.
 * @count: 실제 소비한 바이트 수. get_buffer 반환량보다 작아도 됨.
 * @return: 0 = 성공, 음수 errno = 실패.
 *
 * 왜 필요한가: 파서가 PDU 한 개를 다 처리한 뒤 그 만큼만 진행 → 다음 데이터로 넘어감.
 *              writer가 그 영역을 다시 쓸 수 있게 됨 (free space로 환원).
 * 중요한 시맨틱:
 *   - advance 후 직전에 받은 영역은 무효화 — 추가 읽기는 get_buffer 재호출
 *   - pipe가 빈 상태가 되면, group에 속해 있을 경우 **데이터 버퍼가 group 풀로 반납될 수 있음**
 * 동작: read_idx = (read_idx + count) % size, full=false 갱신.
 * 실행 컨텍스트: 단일 스레드 (reader).
 * 호출 체인:
 *   PDU 처리 완료 → [spdk_pipe_reader_advance(consumed)]
 */
int spdk_pipe_reader_advance(struct spdk_pipe *pipe, uint32_t count);

/**
 * Constructs a pipe group.
 *
 * \return spdk_pipe_group. The new pipe group.
 */
/*
 * [한국어]
 * spdk_pipe_group_create - 빈 데이터 버퍼 공유 풀(group) 생성.
 *
 * @return: 새 group 포인터, 실패 시 NULL.
 *
 * 왜 필요한가: 수많은 연결(예: 수천 개의 TCP qpair)이 각각 read 버퍼를 가지고 있지만 동시에 모두
 *              데이터를 다루지는 않음. 빈 상태인 pipe들이 자신의 데이터 버퍼를 그룹 풀에 반납하면,
 *              실제로 데이터가 도착한 pipe만 풀에서 빌려 쓰는 방식으로 메모리 사용을 최적화.
 *              스택(LIFO) 구조로 가장 최근 반납 버퍼를 우선 재사용 → CPU 캐시 친화적.
 * 동작: group 메타 구조체 malloc, 빈 스택 초기화.
 * 실행 컨텍스트: 초기화 경로.
 * 호출 체인:
 *   sock group / TCP listener init → [spdk_pipe_group_create]
 */
struct spdk_pipe_group *spdk_pipe_group_create(void);

/**
 * Destroys the pipe group.
 *
 * \param group The pipe group to operate on.
 */
/*
 * [한국어]
 * spdk_pipe_group_destroy - group 해제.
 *
 * @group: 해제할 group. 호출 전에 모든 pipe가 group_remove로 분리되어 있어야 함.
 *
 * 동작: group 메타 free. 풀에 남은 빈 데이터 버퍼는 free하지 않는다 (소유권 추적이 호출자에 있어야 함).
 * 실행 컨텍스트: 종료 경로.
 */
void spdk_pipe_group_destroy(struct spdk_pipe_group *group);

/**
 * Adds the pipe to the group.
 *
 * When a pipe reaches empty state, it puts the data buffer into
 * the group's pool. If a pipe needs a data buffer, it takes one
 * from the pool. Since the pool is a stack, a small number of
 * data buffers tend to be re-used very frequently.
 *
 * \param group The pipe group to operate on.
 * \param pipe The pipe to be added.
 *
 * \return On error, a negated errno. On success, 0.
 */
/*
 * [한국어]
 * spdk_pipe_group_add - pipe를 group에 가입. 이후 빈 상태 시 버퍼를 자동 반납하는 동작.
 *
 * @group: 가입할 그룹.
 * @pipe:  추가할 pipe.
 * @return: 0 = 성공. 음수 errno = 실패.
 *
 * 왜 필요한가: pipe가 group 시맨틱(빈 상태 시 버퍼 반납, 필요 시 풀에서 빌림)에 참여하도록 등록.
 * 동작: pipe의 group 포인터를 group으로 세팅. 이후 advance/get_buffer가 group 풀과 상호작용.
 * 실행 컨텍스트: 단일 스레드 — pipe 소유자.
 * 호출 체인:
 *   연결 생성 → spdk_pipe_create → [spdk_pipe_group_add]
 */
int spdk_pipe_group_add(struct spdk_pipe_group *group, struct spdk_pipe *pipe);

/**
 * Removes the pipe to the group.
 *
 * \param group The pipe group to operate on.
 * \param pipe The pipe to be removed.
 *
 * \return On error, a negated errno. On success, 0.
 */
/*
 * [한국어]
 * spdk_pipe_group_remove - pipe를 group에서 분리.
 *
 * @group: 그룹.
 * @pipe:  제거할 pipe.
 * @return: 0 = 성공. 음수 errno = 실패.
 *
 * 왜 필요한가: pipe destroy 전, 또는 그룹 시맨틱이 더 이상 필요 없을 때 안전하게 분리.
 * 동작:
 *   1. pipe가 현재 풀에서 빌린 버퍼가 있다면 처리 (그대로 보유)
 *   2. pipe의 group 포인터를 NULL로 세팅 → 이후 빈 상태가 되어도 풀에 반납하지 않음
 * 호출 체인:
 *   연결 종료 → [spdk_pipe_group_remove] → spdk_pipe_destroy
 */
int spdk_pipe_group_remove(struct spdk_pipe_group *group, struct spdk_pipe *pipe);

#ifdef __cplusplus
/* [한국어] C++ extern "C" 블록 닫기. */
}
#endif

#endif
/* [한국어] SPDK_PIPE_H 헤더 가드 종료. */
