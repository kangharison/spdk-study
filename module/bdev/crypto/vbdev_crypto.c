/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * [한국어 설명] crypto vbdev (virtual block device) 코어 구현 (vbdev_crypto.c)
 *
 * === 파일의 역할 ===
 * crypto vbdev 모듈은 임의의 base bdev 위에 "투명한 블록 디바이스 암호화"
 * 레이어를 덮어 노출하는 가상 bdev 이다. 사용자가 crypto vbdev 에 일반 read/write
 * 요청을 보내면, 이 파일이 그 I/O 를 가로채 SPDK accel framework 를 통해 AES-XTS
 * 등 대칭키 암호화/복호화를 수행한 뒤 하위 base bdev 로 전달하거나, 하위 base bdev
 * 에서 읽어온 결과를 복호화한 뒤 사용자에게 돌려준다. 핵심 설계 포인트는 (1) crypto
 * 알고리즘 자체는 직접 구현하지 않고 spdk_accel_submit_encrypt/decrypt 에 위임하여
 * SW(software) / DPDK CryptoDev / NVIDIA mlx5 등 어느 backend 든 동일하게 사용 가능,
 * (2) DEK(Data Encryption Key) 는 lib/keyring 에 로드된 spdk_accel_crypto_key 로
 * 참조 — 키 자체를 vbdev 내부에 복제하지 않고 ref-count, (3) 사용자가 건넨 write 버퍼를
 * in-place 로 변조하지 않고 별도 aux buffer 로 암호화한 결과를 base bdev 에 전달하여
 * 호스트의 원본 데이터를 보존, (4) tweak (IV) 값으로 LBA(Logical Block Address) 를
 * 사용해 같은 평문 블록이 다른 LBA 에 저장될 때 다른 암호문이 되도록 보장하는 AES-XTS
 * 표준 동작을 지킨다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (상위 → 하위):
 *   [Application]
 *     → spdk_bdev_writev/readv (lib/bdev 의 일반 bdev API)
 *     → bdev layer 가 vbdev_crypto 의 fn_table.submit_request 디스패치
 *     → vbdev_crypto_submit_request()  [본 파일]
 *         ├ READ:  spdk_bdev_io_get_buf → crypto_read_get_buf_cb → spdk_accel_append_decrypt
 *         │        → crypto_read → spdk_bdev_readv_blocks_ext (base bdev) → _complete_internal_io
 *         └ WRITE: spdk_accel_get_buf (aux buf) → crypto_encrypt → spdk_accel_append_encrypt
 *                  → crypto_write → spdk_bdev_writev_blocks_ext (base bdev) → _complete_internal_io
 *     → [base bdev module (e.g. bdev_nvme, bdev_aio …)]
 *     → [실제 NVMe/SSD]
 *
 * 실행 컨텍스트: 모든 함수는 spdk_thread (= reactor) 컨텍스트에서 실행되며, lockless
 * 모델을 위해 한 crypto_bdev 의 I/O 는 채널을 소유한 단일 reactor 에서만 처리된다.
 * 단, examine/destruct 시점의 base bdev open/close 는 base bdev 를 처음 연 thread 에서
 * 수행해야 하므로 thread != current 면 spdk_thread_send_msg 로 위임한다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/bdev (spdk_bdev_*): crypto vbdev 자체를 일반 bdev 로 등록 (spdk_bdev_register)
 *   하고, base bdev 에 대해서는 spdk_bdev_open_ext / spdk_bdev_module_claim_bdev 로
 *   소유권을 주장한다. read/write 는 spdk_bdev_*_blocks_ext API 를 사용.
 * - lib/accel: spdk_accel_get_io_channel / spdk_accel_append_encrypt /
 *   spdk_accel_append_decrypt / spdk_accel_get_buf 를 통해 crypto 연산을 위임.
 *   accel "sequence" 모델을 사용해 encrypt + write 또는 read + decrypt 를 하나의
 *   체인으로 묶어 bdev_io 의 accel_sequence 로 base bdev 에 전달 — base bdev 가
 *   메모리 도메인을 지원하면 zero-copy 로 진행되고, 그렇지 않으면 sequence 가
 *   완료된 뒤에 base bdev 가 호출된다.
 * - lib/keyring (spdk_accel_crypto_key_get/destroy): DEK 키를 이름으로 lookup,
 *   crypto vbdev 가 직접 키를 생성한 경우(key_owner=true) destruct 시 destroy.
 * - lib/iobuf (spdk_iobuf_get_opts): aux buffer 크기 제한 (large_bufsize) 을 가져와
 *   max_rw_size 에 반영, 단일 I/O 가 large pool 크기를 넘지 않도록 보장.
 * - vbdev_crypto.h: vbdev_crypto_opts / create_crypto_opts_by_name / free_crypto_opts /
 *   delete_crypto_disk 등을 vbdev_crypto_rpc.c 와 공유.
 *
 * === 주요 함수/구조체 요약 ===
 * 핵심 자료구조:
 *   - struct vbdev_crypto: crypto vbdev 인스턴스 1개 (base_bdev + base_desc + crypto_bdev
 *     + opts 묶음, g_vbdev_crypto TAILQ 에 등록).
 *   - struct crypto_io_channel: per-thread 채널, base bdev 채널 + accel 채널 + DEK 보관.
 *   - struct crypto_bdev_io: bdev_io 별 driver_ctx, aux buffer 와 accel sequence 보관.
 *   - struct bdev_names: 미래에 examine 될 base bdev 이름과 opts 의 association.
 * 핵심 함수:
 *   - vbdev_crypto_submit_request(): I/O 디스패처 (READ/WRITE/UNMAP/FLUSH/RESET).
 *   - crypto_encrypt() / crypto_write(): WRITE 경로 — accel sequence 에 encrypt 를
 *     append 한 뒤 base bdev write 로 sequence 전달.
 *   - crypto_read_get_buf_cb() / crypto_read(): READ 경로 — sequence 에 decrypt 를
 *     append 한 뒤 base bdev read 로 sequence 전달.
 *   - vbdev_crypto_claim(): examine 단계에서 base bdev 와 매칭되는 opts 를 찾아
 *     실제 vbdev 인스턴스를 만들고 spdk_bdev_register 호출.
 *   - vbdev_crypto_examine(): SPDK bdev 가 새로 등장할 때마다 호출되는 콜백.
 *   - delete_crypto_disk(): RPC 로 vbdev 삭제 요청을 처리, unregister_by_name 사용.
 */

#include "vbdev_crypto.h"                  /* [한국어] crypto vbdev 공개 헤더 — vbdev_crypto_opts / create_crypto_opts_by_name / delete_crypto_disk / BDEV_CRYPTO_DEFAULT_CIPHER 등 RPC 파일과 공유되는 인터페이스. */

#include "spdk_internal/assert.h"          /* [한국어] SPDK_UNREACHABLE() 등 내부 assert 매크로 — switch 디폴트에서 도달 불가능한 분기를 표시. */
#include "spdk/thread.h"                   /* [한국어] spdk_thread / spdk_get_thread / spdk_thread_send_msg — cross-thread close 시 base bdev open thread 로 메시지 전달. */
#include "spdk/bdev_module.h"              /* [한국어] bdev 모듈 등록 API (SPDK_BDEV_MODULE_REGISTER, spdk_bdev_fn_table 등) — vbdev 가 자신을 bdev module 로 등록할 때 필요. */
#include "spdk/likely.h"                   /* [한국어] spdk_unlikely() 매크로 — 에러/-ENOMEM 같은 드문 경로를 컴파일러 branch hint 로 표시. */

/* This namespace UUID was generated using uuid_generate() method. */
/* [한국어] crypto vbdev 의 UUID 네임스페이스 (RFC 4122 §4.3 name-based UUID).
 * spdk_uuid_generate_sha1(ns_uuid, base_bdev->uuid) 형태로 crypto vbdev 의 UUID 를
 * 결정론적으로 만들기 위한 시드 — 같은 base bdev 위에 crypto 를 다시 만들면 항상
 * 같은 UUID 가 나오도록 보장 (영속성/식별성 목적). */
#define BDEV_CRYPTO_NAMESPACE_UUID "078e3cf7-f4b4-4545-b2c3-d40045a64ae2"

struct bdev_names {
	/* [한국어] 이 association (vbdev_name ↔ base_bdev_name + DEK) 의 옵션 묶음.
	 * 설정자: vbdev_crypto_insert_name() (RPC bdev_crypto_create 경로).
	 * 읽는 자: vbdev_crypto_claim() — base bdev 가 등장(examine) 했을 때 이 opts 의
	 * bdev_name 과 매칭되면 실제 vbdev 인스턴스 생성에 사용.
	 * 동기화: 모든 mutation 은 단일 SPDK init thread 또는 RPC 처리 thread 에서 발생,
	 * 별도 lock 없음 (RPC 자체가 직렬화). */
	struct vbdev_crypto_opts	*opts;
	TAILQ_ENTRY(bdev_names)		link;
	/* [한국어] g_bdev_names TAILQ 에 연결하는 링크. */
};

/* List of crypto_bdev names and their base bdevs via configuration file. */
/* [한국어] 아직 base bdev 가 등장하지 않은 "예약된" crypto vbdev 들의 목록.
 * RPC bdev_crypto_create 가 호출되면 base bdev 가 실제로 있든 없든 일단 이 리스트에
 * 등록되고, 이후 base bdev 가 examine_config 를 통해 등장하면 매칭되는 항목을 보고
 * 실제 vbdev 인스턴스를 만든다. */
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

struct vbdev_crypto {
	struct spdk_bdev		*base_bdev;		/* the thing we're attaching to */
	/* [한국어] crypto 가 덮어쓰는 하위 base bdev 의 포인터.
	 * 설정자: vbdev_crypto_claim() 에서 spdk_bdev_desc_get_bdev() 결과로 저장.
	 * 읽는 자: io_type_supported / event_cb / destruct 등 base bdev 의 능력/상태를
	 * 위임해서 답할 때 사용.
	 * 동기화: base_desc 가 살아있는 동안 valid 보장 (lib/bdev 의 desc 가 ref 유지). */

	struct spdk_bdev_desc		*base_desc;		/* its descriptor we get from open */
	/* [한국어] base bdev open descriptor — spdk_bdev_open_ext() 의 산출물.
	 * 설정자: vbdev_crypto_claim() 에서 open_ext 호출 후 저장.
	 * 읽는 자: I/O submit 시 spdk_bdev_*_blocks_ext 의 desc 인자로 전달.
	 * 동기화: open/close 는 thread 필드에 기록된 thread 에서만 수행되며,
	 * cross-thread 면 spdk_thread_send_msg 로 _vbdev_crypto_destruct 호출 디스패치. */

	struct spdk_bdev		crypto_bdev;		/* the crypto virtual bdev */
	/* [한국어] 외부에 노출되는 SPDK bdev 구조체 본체 (embedded). spdk_bdev_register 의
	 * 대상이며, SPDK_CONTAINEROF(bdev, struct vbdev_crypto, crypto_bdev) 로 역참조.
	 * 동기화: 필드 mutation 은 init 단계에서만, 이후엔 read-only 처럼 다뤄짐. */

	struct vbdev_crypto_opts	*opts;			/* crypto options such as names and DEK */
	/* [한국어] 이 vbdev 의 옵션 (bdev_name / vbdev_name / DEK key / key_owner 등).
	 * 설정자: vbdev_crypto_claim() 에서 name->opts 포인터를 그대로 차용 (소유권 이전).
	 * 읽는 자: dump_info_json / config_json / 채널 생성 시 DEK 참조.
	 * 동기화: opts 의 lifetime 은 vbdev unregister 이후 vbdev_crypto_delete_name 에서 해제. */

	TAILQ_ENTRY(vbdev_crypto)	link;
	/* [한국어] g_vbdev_crypto 글로벌 리스트에 연결하는 링크. */

	struct spdk_thread		*thread;		/* thread where base device is opened */
	/* [한국어] base bdev 를 처음 open 한 thread — close 도 같은 thread 에서 해야 함.
	 * destruct 시 spdk_get_thread() 와 비교해 cross-thread 면 spdk_thread_send_msg
	 * 로 위임. SPDK bdev open/close 는 desc 를 만든 thread 에서만 안전. */
};

/* List of virtual bdevs and associated info for each. We keep the device friendly name here even
 * though its also in the device struct because we use it early on.
 */
/* [한국어] 현재 시스템에 활성화된 crypto vbdev 인스턴스들의 글로벌 리스트.
 * hotremove/resize 콜백, config_json dump 시 전체 순회용. */
static TAILQ_HEAD(, vbdev_crypto) g_vbdev_crypto = TAILQ_HEAD_INITIALIZER(g_vbdev_crypto);

/* The crypto vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * We store things in here that are needed on per thread basis like the base_channel for this thread.
 */
struct crypto_io_channel {
	struct spdk_io_channel		*base_ch;	/* IO channel of base device */
	/* [한국어] 이 thread 의 base bdev I/O 채널.
	 * 설정자: crypto_bdev_ch_create_cb() 가 spdk_bdev_get_io_channel(base_desc) 호출.
	 * 읽는 자: crypto_read/write 등 base bdev 로 I/O 를 submit 할 때 ch 인자로 사용.
	 * 동기화: per-thread 객체 — 다른 thread 가 접근하지 않음. */

	struct spdk_io_channel		*accel_channel;	/* Accel engine channel used for crypto ops */
	/* [한국어] 이 thread 의 accel framework 채널 (lib/accel).
	 * 설정자: crypto_bdev_ch_create_cb() 에서 spdk_accel_get_io_channel() 호출.
	 * 읽는 자: spdk_accel_append_encrypt/decrypt / spdk_accel_get_buf 등에 전달.
	 * 동기화: per-thread, lockless. */

	struct spdk_accel_crypto_key	*crypto_key;
	/* [한국어] 이 vbdev 의 DEK (Data Encryption Key) 핸들 — keyring 에서 lookup 된 ref.
	 * 설정자: 채널 생성 시 vbdev->opts->key 를 캐시 (per-thread 빠른 접근 목적).
	 * 읽는 자: spdk_accel_append_encrypt/decrypt 의 key 인자.
	 * 동기화: key 자체는 keyring 이 ref-count 관리, 채널 lifetime 동안 valid. */
};

enum crypto_io_resubmit_state {
	CRYPTO_IO_DECRYPT_DONE,	/* Appended decrypt, need to read */
	/* [한국어] READ 경로에서 decrypt 가 이미 sequence 에 append 된 상태 —
	 * spdk_bdev_readv_blocks_ext 호출이 -ENOMEM 으로 실패하여 queue_io 로 대기 중.
	 * resubmit 시 crypto_read() 부터 다시 호출해 base bdev read 만 재시도. */

	CRYPTO_IO_ENCRYPT_DONE,	/* Need to write */
	/* [한국어] WRITE 경로에서 encrypt 가 이미 sequence 에 append 된 상태 —
	 * spdk_bdev_writev_blocks_ext 가 -ENOMEM 으로 실패하여 queue_io 로 대기 중.
	 * resubmit 시 crypto_write() 부터 호출해 base bdev write 만 재시도. */
};

/* This is the crypto per IO context that the bdev layer allocates for us opaquely and attaches to
 * each IO for us.
 */
struct crypto_bdev_io {
	struct crypto_io_channel *crypto_ch;		/* need to store for crypto completion handling */
	/* [한국어] 이 I/O 를 issue 한 채널 캐시 — completion 처리에서 다시 ch 를 얻기 위해 보관.
	 * 설정자: vbdev_crypto_submit_request() 진입 시 spdk_io_channel_get_ctx(ch).
	 * 읽는 자: crypto_io_fail / _complete_internal_io 등 콜백 경로. */

	struct vbdev_crypto *crypto_bdev;		/* the crypto node struct associated with this IO */
	/* [한국어] 이 I/O 가 향하는 crypto vbdev 본체. blocklen / base_desc 등 빠른 접근용. */

	/* Used for the single contiguous buffer that serves as the crypto destination target for writes */
	uint64_t aux_num_blocks;			/* num of blocks for the contiguous buffer */
	/* [한국어] aux buffer 가 담는 블록 수 — bdev_io->u.bdev.num_blocks 의 복사본.
	 * 설정자: crypto_encrypt() 에서 설정.
	 * 읽는 자: crypto_write() 의 spdk_bdev_writev_blocks_ext num_blocks 인자. */

	uint64_t aux_offset_blocks;			/* block offset on media */
	/* [한국어] base bdev 상에서 이 암호화 결과를 쓸 시작 LBA.
	 * AES-XTS 의 tweak (IV) 도 같은 LBA 를 사용하므로 일관성이 보장됨. */

	void *aux_buf_raw;				/* raw buffer that the bdev layer gave us for write buffer */
	/* [한국어] spdk_accel_get_buf() 가 할당한 raw 버퍼 포인터 (정렬되지 않은 시작).
	 * 설정자: WRITE 경로에서 spdk_accel_get_buf() 호출 결과.
	 * 읽는 자: spdk_accel_put_buf() 로 해제 시점에 사용.
	 * 값 범위: NULL = aux buf 미할당 (READ 경로 등), non-NULL = 할당된 raw 버퍼. */

	struct iovec aux_buf_iov;			/* iov representing aligned contig write buffer */
	/* [한국어] aux_buf_raw 를 base bdev 의 required_alignment 로 정렬한 iov.
	 * iov_base = (aux_buf_raw + (align-1)) & ~(align-1), iov_len = total_length.
	 * encrypt 결과의 destination 이자 base bdev write 의 source. */

	struct spdk_memory_domain *aux_domain;		/* memory domain of the aux buf */
	/* [한국어] aux buffer 의 메모리 도메인 (host DDR / GPU / DPU 등).
	 * 설정자: spdk_accel_get_buf() 가 두 번째 출력으로 채움.
	 * 읽는 자: spdk_bdev_writev_blocks_ext 의 ext_io_opts.memory_domain 으로 전달
	 * — base bdev (예: RDMA/NVMe-oF) 가 직접 도메인을 인지하면 zero-copy 가 가능. */

	void *aux_domain_ctx;				/* memory domain ctx of the aux buf */
	/* [한국어] memory_domain 과 짝이 되는 context 포인터 (DMA pin/translation 등에 사용). */

	struct spdk_accel_sequence *seq;		/* sequence of accel operations */
	/* [한국어] accel framework 의 "operation 체인" 핸들.
	 * READ: bdev_io 시작 시 외부에서 받은 sequence (NULL 가능) 에 decrypt 를 append.
	 * WRITE: 같은 sequence 에 encrypt 를 append.
	 * 이후 spdk_bdev_*_blocks_ext 의 accel_sequence 옵션으로 전달하면, base bdev 가
	 * 지원하는 경우 in-place 로 sequence 를 실행, 미지원이면 lib/bdev 가 명시적으로
	 * spdk_accel_sequence_finish 후 base I/O 를 진행. */

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	/* [한국어] -ENOMEM 발생 시 base bdev 에 "여유가 생기면 깨워달라" 등록하는 wait entry.
	 * cb_fn = vbdev_crypto_resubmit_io, cb_arg = bdev_io. */

	enum crypto_io_resubmit_state resubmit_state;
	/* [한국어] queue 된 I/O 가 재진입했을 때 어느 단계부터 재시도해야 하는지 표시. */
};

/* [한국어] forward declaration — 콜백/resubmit 경로가 서로 호출하므로 미리 선언. */
static void vbdev_crypto_queue_io(struct spdk_bdev_io *bdev_io,
				  enum crypto_io_resubmit_state state);
static void _complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void vbdev_crypto_examine(struct spdk_bdev *bdev);
static int vbdev_crypto_claim(const char *bdev_name);
static void vbdev_crypto_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);

/*
 * [한국어]
 * crypto_io_fail - aux buffer 해제 + accel sequence abort + bdev_io 를 FAILED 로 완료.
 *
 * @crypto_io: 실패한 I/O 의 driver_ctx (bdev_io 와 1:1 대응).
 *
 * base bdev 로 submit 되기 전에 무엇인가 실패한 경우(예: queue_io 실패, encrypt 실패)
 * 호출된다. base bdev 로 이미 넘어간 후라면 sequence 가 실행/abort 되었을 수 있으므로
 * 이 함수를 사용하면 안 된다 (주석 명시).
 *
 * 단계:
 *   1) aux_buf_raw 가 있으면 spdk_accel_put_buf() 로 반환 (WRITE 경로에서만 alloc).
 *   2) spdk_accel_sequence_abort() — 아직 실행되지 않은 sequence 를 폐기.
 *   3) spdk_bdev_io_complete(... FAILED) — 상위(bdev layer) 로 실패 통지.
 *
 * 실행 컨텍스트: I/O 를 시작한 reactor 와 동일.
 * 호출 체인: crypto_encrypt/crypto_write/crypto_read/queue_io 의 에러 경로 → 본 함수.
 */
static void
crypto_io_fail(struct crypto_bdev_io *crypto_io)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(crypto_io); /* [한국어] driver_ctx → bdev_io 역참조 (lib/bdev 헬퍼). */
	struct crypto_io_channel *crypto_ch = crypto_io->crypto_ch;       /* [한국어] 캐시된 채널 — put_buf 호출에 필요. */

	if (crypto_io->aux_buf_raw) {                                     /* [한국어] WRITE 경로에서만 aux buf 가 할당됨 — 있으면 반드시 반환. */
		spdk_accel_put_buf(crypto_ch->accel_channel, crypto_io->aux_buf_raw,
				   crypto_io->aux_domain, crypto_io->aux_domain_ctx); /* [한국어] accel pool 에 buffer 반환 (memory domain 정보도 함께). */
	}

	/* This function can only be used to fail an IO that hasn't been sent to the base bdev,
	 * otherwise accel sequence might have already been executed/aborted. */
	spdk_accel_sequence_abort(crypto_io->seq);                         /* [한국어] accel sequence 의 모든 pending op 폐기 (NULL 도 안전). */
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);        /* [한국어] 상위 호출자에게 실패 보고. */
}

/*
 * [한국어]
 * crypto_write - 이미 encrypt 가 sequence 에 append 된 상태에서 base bdev 로 write 위임.
 *
 * @crypto_ch: 이 thread 의 crypto 채널 (base bdev 채널 + accel 채널 보관).
 * @bdev_io: 원래 사용자 write 요청.
 *
 * 동기/배경: WRITE 경로의 마지막 단계 — accel sequence(encrypt) 를 ext_io_opts 의
 * accel_sequence 로 base bdev 에 넘기면, base bdev (또는 lib/bdev 의 helper) 가
 * sequence 실행 후 데이터 write 까지 처리해준다. aux_buf_iov 가 source, base bdev
 * 의 aux_offset_blocks 가 destination LBA.
 *
 * 단계:
 *   1) ext_io_opts 에 sequence + aux 의 memory_domain 정보 채움.
 *   2) spdk_bdev_writev_blocks_ext() 호출.
 *   3) -ENOMEM 이면 queue_io(ENCRYPT_DONE) 로 재시도 큐잉, 그 외 에러는 crypto_io_fail.
 *
 * 실행 컨텍스트: I/O 채널 소유 reactor.
 * 호출 체인: vbdev_crypto_submit_request → crypto_encrypt → 본 함수
 *           또는 vbdev_crypto_resubmit_io(ENCRYPT_DONE) → 본 함수.
 */
static void
crypto_write(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);                              /* [한국어] embedded crypto_bdev 로부터 컨테이너 vbdev_crypto 역참조. */
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx; /* [한국어] per-IO driver_ctx 캐스팅. */
	struct spdk_bdev_ext_io_opts opts = {};                                 /* [한국어] 확장 옵션 구조체 (sequence + memory_domain 전달용). */
	int rc;                                                                 /* [한국어] spdk_bdev_writev_blocks_ext() 반환 코드 — 0=제출 성공, -ENOMEM=재시도, 그 외=실패. */

	opts.size = sizeof(opts);                                               /* [한국어] ABI 확장 안전성 — 현재 컴파일된 구조체 크기를 명시. */
	opts.accel_sequence = crypto_io->seq;                                   /* [한국어] encrypt 를 append 한 sequence 를 base bdev 에 위임. */
	opts.memory_domain = crypto_io->aux_domain;                             /* [한국어] aux buffer 의 메모리 도메인 (zero-copy hint). */
	opts.memory_domain_ctx = crypto_io->aux_domain_ctx;                     /* [한국어] domain 별 context (DMA 핸들 등). */

	/* Write the encrypted data. */
	rc = spdk_bdev_writev_blocks_ext(crypto_bdev->base_desc, crypto_ch->base_ch,
					 &crypto_io->aux_buf_iov, 1, crypto_io->aux_offset_blocks,
					 crypto_io->aux_num_blocks, _complete_internal_io,
					 bdev_io, &opts);                       /* [한국어] base bdev 에 write 제출 — sequence 가 함께 실행됨. */
	if (spdk_unlikely(rc != 0)) {                                           /* [한국어] submit 실패 처리 (드문 경로). */
		if (rc == -ENOMEM) {                                            /* [한국어] base bdev 의 ring/리소스 부족 — 대기 후 재시도. */
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n"); /* [한국어] 디버그 로그(component=vbdev_crypto 토글) — NOMEM 으로 큐잉됨을 기록. */
			vbdev_crypto_queue_io(bdev_io, CRYPTO_IO_ENCRYPT_DONE); /* [한국어] encrypt 단계는 끝났으므로 write 부터 재시도. */
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");             /* [한국어] 에러 로그(항상 출력) — 회복 불가한 submit 실패를 알림. */
			crypto_io_fail(crypto_io);                              /* [한국어] 회복 불가 — 상위에 실패 보고. */
		}
	}
}

/* We're either encrypting on the way down or decrypting on the way back. */
/*
 * [한국어]
 * crypto_encrypt - WRITE 경로의 1단계 — accel sequence 에 encrypt 를 append 한 뒤 write 호출.
 *
 * @crypto_ch: 이 thread 의 crypto 채널.
 * @bdev_io: 원래 사용자 write 요청 (iov 와 offset/num_blocks 포함).
 *
 * 동기/배경: 사용자 buffer 를 in-place 로 변조하지 않기 위해 별도의 정렬된 aux buffer
 * 를 destination 으로 두고, accel framework 에 "src(user iovs) → dst(aux_buf_iov)
 * 로 AES-XTS 암호화" 작업을 sequence 에 append 한다. tweak (IV) 로는 LBA(offset_blocks)
 * 를 사용해 NVMe ZNS 동일 평문/다른 위치가 다른 암호문이 되도록 표준 동작을 따른다.
 *
 * 단계:
 *   1) total_length = num_blocks * blocklen, alignment 보정 후 aux_buf_iov 구성.
 *   2) spdk_accel_append_encrypt(seq, key, dst_iov, src_iov, iv=LBA, block_size=blocklen).
 *   3) 실패 시 aux buf 반환 + complete (NOMEM 은 상위가 재시도하도록).
 *   4) 성공 시 crypto_write() 로 진행 — base bdev 에 write submit.
 *
 * 실행 컨텍스트: 채널 reactor.
 * 호출 체인: vbdev_crypto_submit_request (WRITE 분기) → 본 함수 → crypto_write
 *           → spdk_bdev_writev_blocks_ext.
 */
static void
crypto_encrypt(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io)
{
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx; /* [한국어] driver_ctx 캐스팅. */
	uint32_t blocklen = crypto_io->crypto_bdev->crypto_bdev.blocklen;        /* [한국어] crypto vbdev 의 블록 크기 (LBA 단위) — AES-XTS block_size 인자. */
	uint64_t total_length;                                                   /* [한국어] 전체 페이로드 바이트 수. */
	uint64_t alignment;                                                      /* [한국어] base bdev 요구 정렬 — aux iov_base 정렬에 사용. */
	void *aux_buf = crypto_io->aux_buf_raw;                                  /* [한국어] accel pool 에서 받은 raw 시작 주소 (정렬 보장 안 됨). */
	int rc;                                                                 /* [한국어] spdk_accel_append_encrypt() 반환 코드 — 0=op append 성공, -ENOMEM/그 외=실패. */

	/* For encryption, we need to prepare a single contiguous buffer as the encryption
	 * destination, we'll then pass that along for the write after encryption is done.
	 * This is done to avoiding encrypting the provided write buffer which may be
	 * undesirable in some use cases.
	 */
	total_length = bdev_io->u.bdev.num_blocks * blocklen;                    /* [한국어] LBA 수 × 블록 크기 = 암호화 바이트 수. */
	alignment = spdk_bdev_get_buf_align(&crypto_io->crypto_bdev->crypto_bdev); /* [한국어] base bdev 의 정렬 요구 — DMA 등을 위해 필요. */
	crypto_io->aux_buf_iov.iov_len = total_length;                           /* [한국어] iov 의 length 는 전체 페이로드. */
	crypto_io->aux_buf_iov.iov_base  = (void *)(((uintptr_t)aux_buf + (alignment - 1)) & ~
					   (alignment - 1));                     /* [한국어] (raw + (a-1)) & ~(a-1) — alignment 의 멱승수로 올림 정렬. */
	crypto_io->aux_offset_blocks = bdev_io->u.bdev.offset_blocks;            /* [한국어] base bdev 에 write 할 LBA = 원본 요청의 LBA 그대로. */
	crypto_io->aux_num_blocks = bdev_io->u.bdev.num_blocks;                  /* [한국어] write 할 블록 수. */

	rc = spdk_accel_append_encrypt(&crypto_io->seq, crypto_ch->accel_channel,
				       crypto_ch->crypto_key, &crypto_io->aux_buf_iov, 1,
				       crypto_io->aux_domain, crypto_io->aux_domain_ctx,
				       bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       bdev_io->u.bdev.memory_domain,
				       bdev_io->u.bdev.memory_domain_ctx,
				       bdev_io->u.bdev.offset_blocks, blocklen,
				       NULL, NULL);                              /* [한국어] sequence 에 encrypt op 추가 — iv_seed=LBA, block_size=blocklen → AES-XTS tweak 가 LBA 별로 달라짐. */
	if (spdk_unlikely(rc != 0)) {                                            /* [한국어] append 실패 (드문 경로). */
		spdk_accel_put_buf(crypto_ch->accel_channel, crypto_io->aux_buf_raw,
				   crypto_io->aux_domain, crypto_io->aux_domain_ctx); /* [한국어] aux buf 반환 — submit 으로 이어지지 않으므로 leak 방지. */
		if (rc == -ENOMEM) {                                             /* [한국어] accel pool 부족 — 상위(bdev layer) 가 NOMEM 처리. */
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n"); /* [한국어] 디버그 로그 — accel buf/op 부족으로 NOMEM 반환됨을 기록. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM); /* [한국어] bdev layer 가 wait_queue 로 옮김. */
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");              /* [한국어] 에러 로그 — append_encrypt 회복 불가 실패 기록. */
			crypto_io_fail(crypto_io);                               /* [한국어] 그 외 에러는 FAILED 로 종료. */
		}

		return;
	}

	crypto_write(crypto_ch, bdev_io);                                        /* [한국어] sequence 가 잘 준비되었으면 base bdev write 로 진행. */
}

/*
 * [한국어]
 * _complete_internal_io - base bdev I/O 완료 콜백 — 원본 bdev_io 로 결과 전달.
 *
 * @bdev_io: base bdev 가 생성해 완료시킨 자식 I/O.
 * @success: base bdev I/O 의 성공 여부.
 * @cb_arg: spdk_bdev_*_blocks_ext 호출 시 넘긴 원본 bdev_io 포인터.
 *
 * 동기/배경: vbdev 가 base bdev 에 직접 submit 한 경우, base bdev 는 자기 bdev_io 를
 * 새로 만들어 처리하므로 원본/자식이 분리된다. 완료 시 결과를 원본으로 옮기고 자식은
 * free 한다.
 *
 * 단계: aux buf 반환 → orig_io 에 base 의 상태 복사 → 자식 bdev_io free.
 *
 * 호출 체인: base bdev → 본 콜백 → 상위 bdev layer.
 */
static void
_complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;                                  /* [한국어] 원본 사용자 bdev_io. */
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)orig_io->driver_ctx; /* [한국어] 원본의 driver_ctx 에서 aux 메타 가져옴. */
	struct crypto_io_channel *crypto_ch = crypto_io->crypto_ch;             /* [한국어] put_buf 에 필요한 채널. */

	if (crypto_io->aux_buf_raw) {                                           /* [한국어] WRITE 였다면 aux buf 가 있음 — 반환. */
		spdk_accel_put_buf(crypto_ch->accel_channel, crypto_io->aux_buf_raw,
				   crypto_io->aux_domain, crypto_io->aux_domain_ctx);
	}

	spdk_bdev_io_complete_base_io_status(orig_io, bdev_io);                 /* [한국어] base 의 success/nvme_status 등을 원본에 복사 후 complete. */
	spdk_bdev_free_io(bdev_io);                                             /* [한국어] base bdev 가 만든 자식 bdev_io 해제. */
}

/* [한국어] forward declaration — vbdev_crypto_resubmit_io 가 crypto_read 를 호출. */
static void crypto_read(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io);

/*
 * [한국어]
 * vbdev_crypto_resubmit_io - bdev_io_wait 가 깨운 후 재진입할 때 단계별로 재시도.
 *
 * @arg: spdk_bdev_io 포인터 (queue 등록 시 cb_arg 로 저장).
 *
 * 동기/배경: -ENOMEM 으로 base bdev 가 거부했던 I/O 를 자원이 다시 풀렸을 때
 * (lib/bdev 의 io_wait 메커니즘) base bdev 가 본 함수를 호출해 재시도하게 한다.
 * resubmit_state 에 따라 어느 단계부터 다시 시도할지 분기한다.
 *
 * 호출 체인: lib/bdev (io_wait_queue) → 본 함수 → crypto_read 또는 crypto_write.
 */
static void
vbdev_crypto_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;              /* [한국어] 저장된 원본 I/O 복원. */
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;

	switch (crypto_io->resubmit_state) {
	case CRYPTO_IO_ENCRYPT_DONE:                                            /* [한국어] encrypt 는 이미 sequence 에 있음 — write 만 재시도. */
		crypto_write(crypto_io->crypto_ch, bdev_io);
		break;
	case CRYPTO_IO_DECRYPT_DONE:                                            /* [한국어] decrypt 는 이미 sequence 에 있음 — read 만 재시도. */
		crypto_read(crypto_io->crypto_ch, bdev_io);
		break;
	default:
		SPDK_UNREACHABLE();                                              /* [한국어] enum 에 없는 값은 도달 불가 — 컴파일러 힌트. */
	}
}

/*
 * [한국어]
 * vbdev_crypto_queue_io - -ENOMEM 시 base bdev 의 io_wait queue 에 등록해 자원 회복 시 재호출.
 *
 * @bdev_io: 다시 시도해야 할 사용자 I/O.
 * @state: 어느 단계에서 멈췄는지 (ENCRYPT_DONE / DECRYPT_DONE).
 *
 * 동기/배경: SPDK 의 -ENOMEM 패턴 — base bdev 의 자원이 풀리면 cb_fn 으로 깨워주는
 * spdk_bdev_queue_io_wait API 를 사용한다. queue 자체가 실패하면 회복 불가이므로 fail.
 *
 * 호출 체인: crypto_read/crypto_write 의 -ENOMEM 분기 → 본 함수 → (자원 회복 후)
 *           vbdev_crypto_resubmit_io.
 */
static void
vbdev_crypto_queue_io(struct spdk_bdev_io *bdev_io, enum crypto_io_resubmit_state state)
{
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx; /* [한국어] per-IO driver_ctx — wait entry 와 resubmit_state 를 보관할 위치. */
	int rc;                                                                 /* [한국어] spdk_bdev_queue_io_wait() 반환 코드 — 0=등록 성공, !=0=등록 실패(회복 불가). */

	crypto_io->bdev_io_wait.bdev = bdev_io->bdev;                           /* [한국어] wait entry 가 어느 bdev 에 등록되는지 명시 (lib/bdev 요구). */
	crypto_io->bdev_io_wait.cb_fn = vbdev_crypto_resubmit_io;               /* [한국어] 깨어날 때 호출될 콜백. */
	crypto_io->bdev_io_wait.cb_arg = bdev_io;                               /* [한국어] 콜백 인자 = 원본 bdev_io. */
	crypto_io->resubmit_state = state;                                      /* [한국어] resubmit 분기 결정에 사용. */

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, crypto_io->crypto_ch->base_ch,
				     &crypto_io->bdev_io_wait);                  /* [한국어] base bdev 의 wait queue 에 등록. */
	if (rc != 0) {                                                          /* [한국어] queue 실패 (예: 이미 다른 곳에 enqueue 되어 있음) — 회복 불가. */
		SPDK_ERRLOG("Queue io failed in vbdev_crypto_queue_io, rc=%d.\n", rc); /* [한국어] 에러 로그 — io_wait 등록 실패 사유(rc) 기록. */
		crypto_io_fail(crypto_io);                                      /* [한국어] 재시도 불가 — 상위에 FAILED 보고. */
	}
}

/*
 * [한국어]
 * crypto_read - READ 경로의 마지막 단계 — sequence(decrypt) 와 함께 base bdev read 호출.
 *
 * @crypto_ch: 이 thread 의 crypto 채널.
 * @bdev_io: 원래 사용자 read 요청.
 *
 * 동기/배경: 사용자 iov 를 그대로 read 의 destination 으로 사용 (in-place decrypt).
 * base bdev 가 데이터를 사용자 iov 로 읽어오고, accel sequence 의 decrypt 가 같은
 * iov 에 대해 in-place 복호화를 수행한다 (lib/bdev 가 sequence 실행 시점을 조정).
 *
 * 단계: ext_io_opts 에 sequence + 사용자 메모리 도메인 설정 → readv_blocks_ext 호출.
 *
 * 호출 체인: vbdev_crypto_submit_request → spdk_bdev_io_get_buf → crypto_read_get_buf_cb
 *           → 본 함수 또는 resubmit_io(DECRYPT_DONE) → 본 함수.
 */
static void
crypto_read(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io)
{
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx; /* [한국어] per-IO driver_ctx — seq(decrypt sequence)/crypto_ch 보관처. */
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);                              /* [한국어] container_of 로 vbdev_crypto 역참조. */
	struct spdk_bdev_ext_io_opts opts = {};                                  /* [한국어] sequence 전달용 확장 옵션. */
	int rc;                                                                  /* [한국어] spdk_bdev_readv_blocks_ext() 반환 코드 — 0=제출 성공, -ENOMEM=재시도, 그 외=실패. */

	opts.size = sizeof(opts);                                                /* [한국어] ABI 안정성. */
	opts.accel_sequence = crypto_io->seq;                                    /* [한국어] decrypt 가 append 된 sequence. */
	opts.memory_domain = bdev_io->u.bdev.memory_domain;                      /* [한국어] 사용자가 지정한 메모리 도메인 그대로 전달. */
	opts.memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;              /* [한국어] 메모리 도메인의 짝 context (DMA pin/translation 핸들). */

	rc = spdk_bdev_readv_blocks_ext(crypto_bdev->base_desc, crypto_ch->base_ch,
					bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
					_complete_internal_io, bdev_io, &opts);  /* [한국어] base bdev 에 read submit — sequence 가 함께 처리됨. */
	if (rc != 0) {
		if (rc == -ENOMEM) {                                             /* [한국어] base 자원 부족 — 재시도 큐잉. */
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n"); /* [한국어] 디버그 로그 — base bdev read 가 NOMEM 으로 큐잉됨을 기록. */
			vbdev_crypto_queue_io(bdev_io, CRYPTO_IO_DECRYPT_DONE);  /* [한국어] decrypt 는 이미 sequence 에 있으므로 read 부터 재시도. */
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");              /* [한국어] 에러 로그 — base read 회복 불가 실패. */
			crypto_io_fail(crypto_io);                               /* [한국어] 상위에 FAILED 보고. */
		}
	}
}

/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it.
 */
/*
 * [한국어]
 * crypto_read_get_buf_cb - READ 시작 직전, sequence 에 decrypt 를 append 한 뒤 read 호출.
 *
 * @ch: crypto vbdev 의 IO 채널 (사용자가 bdev layer 에 넘긴 채널).
 * @bdev_io: 사용자 read 요청.
 * @success: spdk_bdev_io_get_buf() 의 buffer 확보 성공 여부.
 *
 * 동기/배경: 사용자가 NULL iov 로 read 를 요청한 경우 bdev layer 가 large buf pool
 * 에서 버퍼를 빌려와 iov 를 채운 뒤 본 콜백을 부른다. 본 콜백은 이 시점에 decrypt
 * op 를 sequence 에 append (src/dst 모두 사용자 iov 로 동일 = in-place) 한 뒤
 * crypto_read() 를 호출해 실제 base bdev read 로 진행한다.
 *
 * 호출 체인: spdk_bdev_io_get_buf → 본 함수 → spdk_accel_append_decrypt → crypto_read.
 */
static void
crypto_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		       bool success)
{
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);       /* [한국어] 채널의 ctx (= struct crypto_io_channel) 추출. */
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx; /* [한국어] per-IO driver_ctx — seq 와 crypto_bdev 캐시 보관처. */
	uint32_t blocklen = crypto_io->crypto_bdev->crypto_bdev.blocklen;        /* [한국어] AES-XTS block_size. */
	int rc;                                                                  /* [한국어] spdk_accel_append_decrypt() 반환 코드 — 0=op append 성공, -ENOMEM/그 외=실패. */

	if (!success) {                                                          /* [한국어] buf 확보 실패 — 진행 불가. */
		crypto_io_fail(crypto_io);                                       /* [한국어] 버퍼를 못 받았으니 즉시 실패 처리. */
		return;                                                          /* [한국어] 더 진행하지 않음. */
	}

	rc = spdk_accel_append_decrypt(&crypto_io->seq, crypto_ch->accel_channel,
				       crypto_ch->crypto_key,
				       bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       bdev_io->u.bdev.memory_domain,
				       bdev_io->u.bdev.memory_domain_ctx,
				       bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       bdev_io->u.bdev.memory_domain,
				       bdev_io->u.bdev.memory_domain_ctx,
				       bdev_io->u.bdev.offset_blocks, blocklen,
				       NULL, NULL);                              /* [한국어] in-place decrypt: src/dst 모두 사용자 iov. tweak = LBA. */
	if (rc != 0) {                                                            /* [한국어] decrypt op append 실패 처리. */
		if (rc == -ENOMEM) {                                             /* [한국어] accel pool 부족 — bdev layer 가 NOMEM 재시도. */
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n"); /* [한국어] 디버그 로그 — append_decrypt NOMEM 기록. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM); /* [한국어] bdev layer 재시도 위임. */
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");              /* [한국어] 에러 로그 — append_decrypt 회복 불가 실패. */
			crypto_io_fail(crypto_io);                               /* [한국어] 상위에 FAILED 보고. */
		}

		return;                                                          /* [한국어] 실패 시 read 단계로 진행하지 않음. */
	}

	crypto_read(crypto_ch, bdev_io);                                         /* [한국어] sequence 준비 완료 — base bdev read 진행. */
}

/* Called when someone submits IO to this crypto vbdev. For IO's not relevant to crypto,
 * we're simply passing it on here via SPDK IO calls which in turn allocate another bdev IO
 * and call our cpl callback provided below along with the original bdev_io so that we can
 * complete it once this IO completes. For crypto operations, we'll either encrypt it first
 * (writes) then call back into bdev to submit it or we'll submit a read and then catch it
 * on the way back for decryption.
 */
/*
 * [한국어]
 * vbdev_crypto_submit_request - crypto vbdev 의 I/O 디스패처 (fn_table.submit_request).
 *
 * @ch: 이 thread 의 crypto vbdev IO 채널.
 * @bdev_io: 처리할 I/O 요청.
 *
 * 동기/배경: lib/bdev 가 사용자 I/O 를 vbdev 의 fn_table 을 통해 전달하면, 본 함수가
 * 타입별 분기를 수행한다. crypto 와 관련된 READ/WRITE 는 accel sequence 경로로 보내고,
 * UNMAP/FLUSH/RESET 같은 메타 I/O 는 그대로 base bdev 에 위임한다. WRITE_ZEROES 는
 * 일부러 지원 안 함으로 보고 (io_type_supported 에서 false 반환) — bdev layer 가
 * 일반 write 로 분해해 보내면 그것을 정상 암호화할 수 있도록 유도.
 *
 * 단계: driver_ctx 초기화 → bdev_io->u.bdev.accel_sequence 인계 → switch 디스패치 →
 *       에러 발생 시 NOMEM/FAIL 처리.
 *
 * 실행 컨텍스트: 채널 reactor (lockless).
 * 호출 체인: 사용자 spdk_bdev_*v_blocks → lib/bdev → 본 함수.
 */
static void
vbdev_crypto_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);                              /* [한국어] vbdev_crypto 본체. */
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);       /* [한국어] 채널 ctx. */
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx; /* [한국어] per-IO driver_ctx — 이번 요청의 crypto 상태를 담는 스크래치. */
	int rc = 0;                                                              /* [한국어] 각 분기의 base bdev/accel submit 반환 코드 누적 — 0=성공으로 초기화. */

	memset(crypto_io, 0, sizeof(struct crypto_bdev_io));                     /* [한국어] driver_ctx 의 모든 필드 0 초기화 — 이전 I/O 잔재 제거. */
	crypto_io->crypto_bdev = crypto_bdev;                                    /* [한국어] 빠른 역참조 캐시. */
	crypto_io->crypto_ch = crypto_ch;                                        /* [한국어] completion 시 채널 복원용으로 보관. */
	crypto_io->seq = bdev_io->u.bdev.accel_sequence;                         /* [한국어] 상위가 이미 만든 sequence 가 있으면 인계 (체이닝). */

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:                                             /* [한국어] READ: get_buf → decrypt append → base read. */
		spdk_bdev_io_get_buf(bdev_io, crypto_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen); /* [한국어] iov 가 비어있으면 large pool 에서 채우고 콜백 호출. */
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:                                            /* [한국어] WRITE: aux buf 확보 → encrypt → base write. */
		/* For encryption we don't want to encrypt the data in place as the host isn't
		 * expecting us to mangle its data buffers so we need to encrypt into the aux accel
		 * buffer, then we can use that as the source for the disk data transfer.
		 */
		rc = spdk_accel_get_buf(crypto_ch->accel_channel,
					bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
					&crypto_io->aux_buf_raw, &crypto_io->aux_domain,
					&crypto_io->aux_domain_ctx);             /* [한국어] aux buffer 확보 (memory_domain 출력 포함). */
		if (rc == 0) {
			crypto_encrypt(crypto_ch, bdev_io);                      /* [한국어] 성공 시에만 다음 단계로. 실패 시 rc 가 아래 if 에서 처리. */
		}
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:                                            /* [한국어] UNMAP: 암호화 무관 — base bdev 로 패스스루. */
		rc = spdk_bdev_unmap_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:                                            /* [한국어] FLUSH: 메타 — 패스스루. */
		rc = spdk_bdev_flush_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:                                            /* [한국어] RESET: 컨트롤러 리셋도 패스스루. */
		rc = spdk_bdev_reset(crypto_bdev->base_desc, crypto_ch->base_ch,
				     _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:                                     /* [한국어] WZ 는 io_type_supported 에서 false 였으므로 여기 오면 안 됨. */
	default:
		SPDK_ERRLOG("crypto: unknown I/O type %d\n", bdev_io->type);     /* [한국어] 에러 로그 — 지원하지 않는 I/O 타입 진입 (방어적 처리). */
		rc = -EINVAL;                                                    /* [한국어] 잘못된 인자 — 아래 통합 에러 처리에서 FAILED. */
		break;
	}

	if (rc != 0) {                                                           /* [한국어] 어떤 분기든 submit 실패 처리 통일. */
		if (rc == -ENOMEM) {                                            /* [한국어] base/accel 자원 부족 — 재시도 가능. */
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n"); /* [한국어] 디버그 로그 — 어떤 분기든 NOMEM 진입 기록. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM); /* [한국어] bdev layer 가 wait queue 로 옮김. */
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");             /* [한국어] 에러 로그 — 회복 불가 submit 실패. */
			crypto_io_fail(crypto_io);                              /* [한국어] 상위에 FAILED 보고. */
		}
	}
}

/* We'll just call the base bdev and let it answer except for WZ command which
 * we always say we don't support so that the bdev layer will actually send us
 * real writes that we can encrypt.
 */
/*
 * [한국어]
 * vbdev_crypto_io_type_supported - 이 vbdev 가 지원하는 I/O 타입을 bdev layer 에 알림.
 *
 * @ctx: vbdev_crypto 본체.
 * @io_type: 질의할 I/O 타입.
 * @return: 지원하면 true, 아니면 false.
 *
 * 동기/배경: 대부분의 I/O 는 base bdev 의 능력을 그대로 위임한다. 단, WRITE_ZEROES 만은
 * 명시적으로 false 를 반환해 bdev layer 가 실제 zero-filled write 로 분해하도록 강제 —
 * 이렇게 해야 zero 데이터도 정상적으로 암호화되어 디스크에 기록된다.
 *
 * 호출 체인: lib/bdev → 본 함수.
 */
static bool
vbdev_crypto_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;            /* [한국어] io_type_supported 의 ctx = vbdev_crypto 본체. */

	switch (io_type) {                                                       /* [한국어] 질의된 I/O 타입별 지원 여부 분기. */
	case SPDK_BDEV_IO_TYPE_WRITE:                                            /* [한국어] 아래 5개는 base 위임. */
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return spdk_bdev_io_type_supported(crypto_bdev->base_bdev, io_type); /* [한국어] base bdev 가 지원하면 vbdev 도 지원 (능력 위임). */
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:                                     /* [한국어] WZ 명시적 미지원 — bdev layer 가 실제 write 로 분해하게 함. */
	/* Force the bdev layer to issue actual writes of zeroes so we can
	 * encrypt them as regular writes.
	 */
	default:
		return false;
	}
}

/* Callback for unregistering the IO device. */
/*
 * [한국어]
 * _device_unregister_cb - spdk_io_device_unregister 완료 콜백 — 메모리 해제 + destruct_done.
 *
 * @io_device: vbdev_crypto 본체.
 *
 * 동기/배경: io_device 등록 해제가 모든 채널 정리 후 완료되었을 때 호출되며,
 * 이 시점에 vbdev 의 마지막 정리 (opts NULL, name free, struct free) 를 수행하고
 * lib/bdev 에 destruct 완료를 통지한다.
 */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_crypto *crypto_bdev = io_device;

	/* Done with this crypto_bdev. */
	crypto_bdev->opts = NULL;                                                /* [한국어] opts 소유권 분리 — vbdev_crypto_delete_name 에서 해제됨. */

	spdk_bdev_destruct_done(&crypto_bdev->crypto_bdev, 0);                   /* [한국어] bdev layer 에 "destruct 완료" 통지 (async destruct 의 마지막 단계). */
	free(crypto_bdev->crypto_bdev.name);                                     /* [한국어] strdup 으로 할당한 이름 해제. */
	free(crypto_bdev);                                                       /* [한국어] vbdev 본체 해제. */
}

/* Wrapper for the bdev close operation. */
/*
 * [한국어]
 * _vbdev_crypto_destruct - spdk_thread_send_msg 로 cross-thread close 를 디스패치할 때 사용하는 trampoline.
 *
 * @ctx: spdk_bdev_desc 포인터.
 */
static void
_vbdev_crypto_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);                                                   /* [한국어] open 했던 thread 에서 close 호출. */
}

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
/*
 * [한국어]
 * vbdev_crypto_destruct - bdev layer 의 unregister 후 호출되는 destruct 엔트리포인트.
 *
 * @ctx: vbdev_crypto 본체.
 * @return: 1 = async destruct (실제 완료는 _device_unregister_cb 에서).
 *
 * 단계:
 *   1) g_vbdev_crypto 에서 제거.
 *   2) base bdev 소유권 해제 (module_release_bdev).
 *   3) base bdev close — open 한 thread 에서 (필요 시 send_msg 로 cross-thread).
 *   4) io_device unregister 트리거 → _device_unregister_cb 비동기 완료.
 *
 * 호출 체인: spdk_bdev_unregister → lib/bdev → 본 함수.
 */
static int
vbdev_crypto_destruct(void *ctx)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	/* Remove this device from the internal list */
	TAILQ_REMOVE(&g_vbdev_crypto, crypto_bdev, link);                        /* [한국어] 활성 vbdev 리스트에서 제거. */

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(crypto_bdev->base_bdev);                   /* [한국어] base bdev 의 module claim 해제 (이제 다른 모듈이 사용 가능). */

	/* Close the underlying bdev on its same opened thread. */
	if (crypto_bdev->thread && crypto_bdev->thread != spdk_get_thread()) {   /* [한국어] open thread != current → cross-thread close 필요. */
		spdk_thread_send_msg(crypto_bdev->thread, _vbdev_crypto_destruct, crypto_bdev->base_desc); /* [한국어] base bdev open thread 로 메시지 디스패치. */
	} else {
		spdk_bdev_close(crypto_bdev->base_desc);                         /* [한국어] same-thread 일 땐 직접 close. */
	}

	/* Unregister the io_device. */
	spdk_io_device_unregister(crypto_bdev, _device_unregister_cb);           /* [한국어] 채널 정리 후 _device_unregister_cb 가 호출됨. */

	return 1;                                                                /* [한국어] async destruct: 호출자는 destruct_done 콜백을 기다림. */
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel. We are passed the same context we provided when
 * we created our crypto vbdev in examine() which, for this bdev, is the address of one of
 * our context nodes. From here we'll ask the SPDK channel code to fill out our channel
 * struct and we'll keep it in our crypto node.
 */
/*
 * [한국어]
 * vbdev_crypto_get_io_channel - fn_table.get_io_channel 엔트리 — per-thread 채널 획득.
 *
 * @ctx: vbdev_crypto 본체.
 * @return: SPDK io_channel — crypto_bdev_ch_create_cb 가 채운 crypto_io_channel 을 감싸는 핸들.
 *
 * 동기/배경: lib/bdev 가 desc 를 처음 open 한 thread 에서 채널을 요청할 때 호출된다.
 * SPDK io_channel 인프라가 채널 생성/캐싱을 처리하며, 새로 생성될 때 crypto_bdev_ch_create_cb
 * 가 호출돼 base bdev 채널 + accel 채널을 셋업한다.
 */
static struct spdk_io_channel *
vbdev_crypto_get_io_channel(void *ctx)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK channel structure plus the size of our crypto_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	return spdk_get_io_channel(crypto_bdev);                                 /* [한국어] io_device key 로 채널 lookup/create. */
}

/* This is the output for bdev_get_bdevs() for this vbdev */
/*
 * [한국어]
 * vbdev_crypto_dump_info_json - fn_table.dump_info_json — bdev_get_bdevs 응답에 crypto 정보 첨부.
 *
 * @ctx: vbdev_crypto 본체.
 * @w: JSON writer.
 * @return: 0 (성공).
 *
 * 동기/배경: SPDK RPC bdev_get_bdevs 가 각 bdev 의 product-specific 정보를 요청할 때
 * 호출된다. base bdev 이름, 자신의 이름, 사용된 DEK 이름을 노출 — 키 자체는 노출 안 함.
 */
static int
vbdev_crypto_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;            /* [한국어] dump_info_json 의 ctx = 등록 시 넘긴 vbdev_crypto 본체. */

	spdk_json_write_name(w, "crypto");                                       /* [한국어] "crypto" 라는 product-specific 필드 시작. */
	spdk_json_write_object_begin(w);                                         /* [한국어] JSON 객체 '{' 시작 — base/name/key_name 을 담는다. */
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev)); /* [한국어] "base_bdev_name": 하위 base bdev 이름. */
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev)); /* [한국어] "name": 이 crypto vbdev 의 노출 이름. */
	spdk_json_write_named_string(w, "key_name", crypto_bdev->opts->key->param.key_name); /* [한국어] keyring 의 키 이름만 노출 (실제 key material 은 절대 dump 안 함). */
	spdk_json_write_object_end(w);                                           /* [한국어] JSON 객체 '}' 종료. */

	return 0;                                                                /* [한국어] dump_info_json 은 항상 성공 — 0 반환. */
}

/*
 * [한국어]
 * vbdev_crypto_config_json - module.config_json — save_config 시 RPC 재현 가능한 JSON 생성.
 *
 * @w: JSON writer.
 *
 * 동기/배경: SPDK 의 save_config RPC 가 호출되면 각 모듈이 자신의 설정을 RPC 명령
 * 시퀀스 형태로 출력해 그대로 load_config 에서 재현 가능하도록 한다. 본 함수는
 * 활성 crypto vbdev 들에 대해 bdev_crypto_create RPC 호출을 직렬화한다.
 */
static int
vbdev_crypto_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_crypto *crypto_bdev;                                        /* [한국어] g_vbdev_crypto 순회 커서. */

	TAILQ_FOREACH(crypto_bdev, &g_vbdev_crypto, link) {                      /* [한국어] 활성 vbdev 전체 순회. */
		spdk_json_write_object_begin(w);                                /* [한국어] 하나의 RPC 호출을 표현하는 JSON 객체 시작. */
		spdk_json_write_named_string(w, "method", "bdev_crypto_create"); /* [한국어] RPC 메서드 이름 — load_config 시 그대로 재호출됨. */
		spdk_json_write_named_object_begin(w, "params");                /* [한국어] "params": { ... } RPC 인자 객체 시작. */
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev)); /* [한국어] 재생성 시 어느 base 위에 올릴지. */
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev)); /* [한국어] 재생성될 crypto vbdev 이름. */
		spdk_json_write_named_string(w, "key_name", crypto_bdev->opts->key->param.key_name); /* [한국어] 사용할 DEK 의 keyring 키 이름 (material 아님). */
		spdk_json_write_object_end(w);                                  /* [한국어] "params" 객체 종료. */
		spdk_json_write_object_end(w);                                  /* [한국어] RPC 호출 객체 종료. */
	}
	return 0;                                                                /* [한국어] config_json 은 항상 성공 — 0 반환. */
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis. We also register the
 * poller used to complete crypto operations from the device.
 */
/*
 * [한국어]
 * crypto_bdev_ch_create_cb - per-thread 채널 생성 콜백 — base+accel 채널 획득.
 *
 * @io_device: vbdev_crypto 본체.
 * @ctx_buf: 채널 ctx 버퍼 (= struct crypto_io_channel, register 시 크기 지정함).
 * @return: 0=성공, -ENOMEM=하위 채널 획득 실패.
 *
 * 단계:
 *   1) base bdev 채널 획득 (spdk_bdev_get_io_channel).
 *   2) accel framework 채널 획득 (spdk_accel_get_io_channel).
 *   3) DEK 키 핸들 캐싱.
 *   실패 시 이미 획득한 채널은 put 으로 롤백.
 *
 * 호출 체인: lib/bdev → spdk_io_device 인프라 → 본 함수.
 */
static int
crypto_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct crypto_io_channel *crypto_ch = ctx_buf;                           /* [한국어] register 시 지정한 sizeof(crypto_io_channel) 영역. */
	struct vbdev_crypto *crypto_bdev = io_device;                            /* [한국어] io_device 등록 키 = vbdev_crypto 포인터. */

	crypto_ch->base_ch = spdk_bdev_get_io_channel(crypto_bdev->base_desc);   /* [한국어] base bdev 의 per-thread 채널. */
	if (crypto_ch->base_ch == NULL) {                                        /* [한국어] base 채널 획득 실패 (자원 부족). */
		SPDK_ERRLOG("Failed to get base bdev IO channel (bdev: %s)\n",
			    crypto_bdev->crypto_bdev.name);                      /* [한국어] 에러 로그 — 어느 vbdev 의 base 채널이 실패했는지. */
		return -ENOMEM;                                                  /* [한국어] 채널 생성 실패 → 상위가 채널 할당 포기. */
	}

	crypto_ch->accel_channel = spdk_accel_get_io_channel();                  /* [한국어] accel framework 의 per-thread 채널 (crypto engine 접근용). */
	if (crypto_ch->accel_channel == NULL) {                                  /* [한국어] accel 채널 획득 실패. */
		SPDK_ERRLOG("Failed to get accel IO channel (bdev: %s)\n",
			    crypto_bdev->crypto_bdev.name);                      /* [한국어] 에러 로그 — accel 채널 실패 기록. */
		spdk_put_io_channel(crypto_ch->base_ch);                         /* [한국어] base 채널은 이미 획득했으므로 롤백. */
		return -ENOMEM;                                                  /* [한국어] 부분 실패 정리 후 채널 생성 실패 반환. */
	}

	crypto_ch->crypto_key = crypto_bdev->opts->key;                          /* [한국어] DEK 캐싱 — 매 I/O 마다 opts 까지 따라가지 않도록. */

	return 0;                                                                /* [한국어] base + accel 채널 모두 확보 — 생성 성공. */
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created.
 */
/*
 * [한국어]
 * crypto_bdev_ch_destroy_cb - 채널 파괴 콜백 — base + accel 채널 반환.
 *
 * @io_device: vbdev_crypto 본체 (unused).
 * @ctx_buf: 파괴할 crypto_io_channel.
 *
 * 호출 체인: spdk_put_io_channel → ref 0 도달 시 본 함수.
 */
static void
crypto_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct crypto_io_channel *crypto_ch = ctx_buf;

	spdk_put_io_channel(crypto_ch->base_ch);                                 /* [한국어] base bdev 채널 ref 감소. */
	spdk_put_io_channel(crypto_ch->accel_channel);                           /* [한국어] accel 채널 ref 감소. */
}

/* Create the association from the bdev and vbdev name and insert
 * on the global list. */
/*
 * [한국어]
 * vbdev_crypto_insert_name - g_bdev_names 에 opts 를 새 항목으로 등록 (중복 검사 포함).
 *
 * @opts: 사용자가 채운 vbdev_crypto_opts (vbdev_name 키로 사용됨).
 * @out: 생성된 bdev_names 노드 출력 — 호출자가 추후 cleanup 에 사용.
 * @return: 0=성공, -EEXIST=같은 이름 존재, -ENOMEM=메모리 부족.
 */
static int
vbdev_crypto_insert_name(struct vbdev_crypto_opts *opts, struct bdev_names **out)
{
	struct bdev_names *name;                                                 /* [한국어] 중복 검사 순회 커서 / 새로 할당될 노드. */

	assert(opts);                                                            /* [한국어] 호출자 계약: opts 는 NULL 일 수 없음. */
	assert(out);                                                             /* [한국어] 호출자 계약: 출력 포인터는 NULL 일 수 없음. */

	TAILQ_FOREACH(name, &g_bdev_names, link) {                               /* [한국어] vbdev_name 으로 중복 체크. */
		if (strcmp(opts->vbdev_name, name->opts->vbdev_name) == 0) {      /* [한국어] 동일 vbdev 이름이 이미 등록됨. */
			SPDK_ERRLOG("Crypto bdev %s already exists\n", opts->vbdev_name); /* [한국어] 에러 로그 — 중복 생성 시도. */
			return -EEXIST;                                          /* [한국어] 중복 → 등록 거부. */
		}
	}

	name = calloc(1, sizeof(struct bdev_names));                             /* [한국어] association 노드 할당. */
	if (!name) {                                                             /* [한국어] 노드 할당 실패. */
		SPDK_ERRLOG("Failed to allocate memory for bdev_names.\n");      /* [한국어] 에러 로그 — OOM. */
		return -ENOMEM;                                                  /* [한국어] 메모리 부족 → 실패. */
	}

	name->opts = opts;                                                       /* [한국어] opts 소유권 인계. */
	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);                            /* [한국어] 전역 association 리스트에 추가. */
	*out = name;                                                             /* [한국어] 생성된 노드를 호출자에게 반환 (추후 cleanup 용). */

	return 0;                                                                /* [한국어] 등록 성공. */
}

/*
 * [한국어]
 * free_crypto_opts - opts 멤버 문자열과 opts 자체를 free (key 는 keyring 책임이므로 건드리지 않음).
 *
 * @opts: 해제할 옵션 (NULL 아님 전제).
 *
 * 공개 함수: vbdev_crypto_rpc.c 에서도 호출되어 vbdev_crypto.h 에 prototype 선언.
 */
void
free_crypto_opts(struct vbdev_crypto_opts *opts)
{
	free(opts->bdev_name);                                                   /* [한국어] strdup 한 base bdev 이름 해제. */
	free(opts->vbdev_name);                                                  /* [한국어] strdup 한 vbdev 이름 해제. */
	free(opts);                                                              /* [한국어] opts 구조체 자체 해제 (key 는 keyring 소유이므로 건드리지 않음). */
}

/*
 * [한국어]
 * vbdev_crypto_delete_name - g_bdev_names 에서 노드 제거 + opts 해제 + (소유 시) DEK 파괴.
 *
 * @name: 제거할 association 노드.
 *
 * 동기/배경: key_owner 가 true 인 경우 (이 vbdev 가 키를 생성한 경우) keyring 의
 * DEK 도 함께 파괴한다. 외부에서 lookup 해 온 키라면 그대로 둔다.
 */
static void
vbdev_crypto_delete_name(struct bdev_names *name)
{
	TAILQ_REMOVE(&g_bdev_names, name, link);                                 /* [한국어] 리스트에서 제거. */
	if (name->opts) {                                                        /* [한국어] opts 가 살아있을 때만 해제 (error path 에서 NULL 일 수 있음). */
		if (name->opts->key_owner && name->opts->key) {                  /* [한국어] 이 vbdev 가 키를 만든 경우만 destroy. */
			spdk_accel_crypto_key_destroy(name->opts->key);          /* [한국어] keyring 에서 키 ref 감소/제거. */
		}
		free_crypto_opts(name->opts);                                    /* [한국어] opts 문자열/구조체 해제. */
		name->opts = NULL;                                               /* [한국어] 이중 해제 방지 표시. */
	}
	free(name);                                                              /* [한국어] association 노드 자체 해제. */
}

/* For the named crypto vbdev and the named base bdev, create the crypto opts */
/*
 * [한국어]
 * create_crypto_opts_by_name - vbdev_name + base_bdev_name + key 로 vbdev_crypto_opts 생성.
 *
 * @name: 이 vbdev 의 이름 (상위에서 strdup 됨).
 * @base_bdev_name: 덮어쓸 base bdev 이름.
 * @key: keyring lookup 또는 신규 생성한 DEK.
 * @key_owner: true 면 vbdev 가 키를 만들었으므로 해제도 책임.
 * @return: 새로 할당된 opts 또는 실패 시 NULL (내부에서 부분 해제).
 */
struct vbdev_crypto_opts *
create_crypto_opts_by_name(char *name, char *base_bdev_name, struct spdk_accel_crypto_key *key,
			   bool key_owner)
{
	struct vbdev_crypto_opts *opts = calloc(1, sizeof(*opts));                /* [한국어] opts 구조체 zero-초기화 할당. */

	if (!opts) {                                                             /* [한국어] 할당 실패. */
		return NULL;                                                     /* [한국어] OOM → NULL (호출자가 실패 처리). */
	}

	opts->bdev_name = strdup(base_bdev_name);                                /* [한국어] 입력 인자는 호출자 owned 이므로 복제 보관. */
	if (!opts->bdev_name) {                                                  /* [한국어] base 이름 복제 실패. */
		free_crypto_opts(opts);                                          /* [한국어] 부분 할당된 opts 정리. */
		return NULL;                                                     /* [한국어] 실패 반환. */
	}
	opts->vbdev_name = strdup(name);                                         /* [한국어] vbdev 이름 복제 보관. */
	if (!opts->vbdev_name) {                                                 /* [한국어] vbdev 이름 복제 실패. */
		free_crypto_opts(opts);                                          /* [한국어] bdev_name 까지 포함해 정리. */
		return NULL;                                                     /* [한국어] 실패 반환. */
	}

	opts->key = key;                                                         /* [한국어] DEK 포인터 그대로 보관. */
	opts->key_owner = key_owner;                                             /* [한국어] 소유 여부 기록. */

	return opts;                                                             /* [한국어] 완성된 opts 반환 (소유권은 호출자/insert_name 으로). */
}

/* RPC entry point for crypto creation. */
/*
 * [한국어]
 * create_crypto_disk - 외부(RPC) 진입점: opts 를 g_bdev_names 에 등록하고 즉시 claim 시도.
 *
 * @opts: 호출자가 만든 옵션 (소유권 본 함수가 인계).
 * @return: 0=성공(즉시 claim 또는 deferred), <0=실패.
 *
 * 동기/배경: base bdev 가 아직 존재하지 않으면 -ENODEV 를 0 으로 변환해 "deferred"
 * 상태로 둔다 — 이후 base bdev 가 examine 을 통해 등장하면 자동으로 claim 된다.
 * 그 외 에러는 등록한 name 을 즉시 제거하고 (opts 는 호출자가 free 하도록) 반환.
 */
int
create_crypto_disk(struct vbdev_crypto_opts *opts)
{
	struct bdev_names *name = NULL;                                          /* [한국어] insert_name 이 채워줄 association 노드 핸들. */
	int rc;                                                                  /* [한국어] insert/claim 단계 반환 코드. */

	rc = vbdev_crypto_insert_name(opts, &name);                              /* [한국어] association 노드 등록. */
	if (rc) {                                                                /* [한국어] 중복/OOM 등 등록 실패. */
		return rc;                                                       /* [한국어] opts 는 미인계 상태이므로 호출자가 free 책임. */
	}

	rc = vbdev_crypto_claim(opts->bdev_name);                                /* [한국어] 즉시 claim 시도 (base bdev 가 이미 있으면 vbdev 생성). */
	if (rc == -ENODEV) {                                                     /* [한국어] base bdev 없음 → deferred 로 처리. */
		SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n"); /* [한국어] notice 로그 — base 등장 시 examine 으로 자동 생성됨을 알림. */
		rc = 0;                                                          /* [한국어] deferred 는 성공으로 간주. */
	}

	if (rc) {                                                                /* [한국어] claim 이 실제 실패한 경우 등록한 name 을 되돌림. */
		assert(name != NULL);                                            /* [한국어] insert 성공했으므로 name 은 반드시 유효. */
		/* In case of error we let the caller function to deallocate @opts
		 * since it is its responsibility. Setting name->opts = NULL let's
		 * vbdev_crypto_delete_name() know it does not have to do anything
		 * about @opts.
		 */
		name->opts = NULL;                                                /* [한국어] opts free 책임을 호출자에게 돌려줌. */
		vbdev_crypto_delete_name(name);                                  /* [한국어] association 노드만 제거 (opts 는 보존). */
	}
	return rc;                                                               /* [한국어] 0=성공/deferred, <0=실패. */
}

/* Called at driver init time, parses config file to prepare for examine calls,
 * also fully initializes the crypto drivers.
 */
/*
 * [한국어]
 * vbdev_crypto_init - module.module_init — 현재 별도 초기화 작업 없음.
 *
 * @return: 0 (성공).
 *
 * 동기/배경: 과거 버전에선 DPDK CryptoDev 의 초기화를 여기서 수행했지만,
 * 현재는 lib/accel 이 책임지므로 본 함수는 stub.
 */
static int
vbdev_crypto_init(void)
{
	return 0;                                                                /* [한국어] 별도 초기화 없음 (accel framework 가 crypto 백엔드 책임) — 항상 성공. */
}

/* Called when the entire module is being torn down. */
/*
 * [한국어]
 * vbdev_crypto_finish - module.module_fini — 남은 g_bdev_names 항목 전부 정리.
 */
static void
vbdev_crypto_finish(void)
{
	struct bdev_names *name;                                                 /* [한국어] head 부터 꺼낼 노드 커서. */

	while ((name = TAILQ_FIRST(&g_bdev_names))) {                            /* [한국어] 리스트가 빌 때까지 head 부터 제거. */
		vbdev_crypto_delete_name(name);                                  /* [한국어] 노드 + opts (+소유 시 DEK) 정리. */
	}
}

/* During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
/*
 * [한국어]
 * vbdev_crypto_get_ctx_size - module.get_ctx_size — bdev_io 마다 추가로 받을 driver_ctx 크기 보고.
 *
 * @return: sizeof(struct crypto_bdev_io).
 *
 * 동기/배경: lib/bdev 는 모든 모듈의 ctx_size 중 최댓값을 bdev_io 의 driver_ctx 영역
 * 크기로 잡아둔다. 본 모듈은 crypto_bdev_io 만큼 요청.
 */
static int
vbdev_crypto_get_ctx_size(void)
{
	return sizeof(struct crypto_bdev_io);                                    /* [한국어] bdev_io 당 driver_ctx 로 확보할 바이트 수 = per-IO crypto 상태 크기. */
}

/*
 * [한국어]
 * vbdev_crypto_base_bdev_hotremove_cb - 매칭되는 vbdev 들을 모두 unregister.
 *
 * @bdev_find: 사라진 base bdev.
 */
static void
vbdev_crypto_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_crypto *crypto_bdev, *tmp;

	TAILQ_FOREACH_SAFE(crypto_bdev, &g_vbdev_crypto, link, tmp) {            /* [한국어] SAFE 버전: 순회 중 list mutation 안전. */
		if (bdev_find == crypto_bdev->base_bdev) {
			spdk_bdev_unregister(&crypto_bdev->crypto_bdev, NULL, NULL); /* [한국어] vbdev 도 함께 unregister → destruct → 정리. */
		}
	}
}

/*
 * [한국어]
 * vbdev_crypto_base_bdev_resize_cb - base bdev 크기 변경 시 vbdev 의 blockcnt 도 업데이트.
 */
static void
vbdev_crypto_base_bdev_resize_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_crypto *crypto_bdev;

	TAILQ_FOREACH(crypto_bdev, &g_vbdev_crypto, link) {
		if (bdev_find == crypto_bdev->base_bdev) {
			spdk_bdev_notify_blockcnt_change(&crypto_bdev->crypto_bdev, bdev_find->blockcnt); /* [한국어] 상위 컨슈머에게 크기 변경 통지. */
		}
	}
}

/* Called when the underlying base bdev triggers asynchronous event such as bdev removal. */
/*
 * [한국어]
 * vbdev_crypto_base_bdev_event_cb - spdk_bdev_open_ext 에 전달되는 event 콜백 — REMOVE/RESIZE 디스패치.
 */
static void
vbdev_crypto_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:                                             /* [한국어] base bdev hotplug remove. */
		vbdev_crypto_base_bdev_hotremove_cb(bdev);
		break;
	case SPDK_BDEV_EVENT_RESIZE:                                             /* [한국어] base bdev 크기 변경. */
		vbdev_crypto_base_bdev_resize_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/*
 * [한국어]
 * vbdev_crypto_get_memory_domains - fn_table.get_memory_domains — 이 vbdev 가 자연스럽게 다룰 수 있는 메모리 도메인 목록 보고.
 *
 * @ctx: vbdev_crypto 본체 (unused).
 * @domains: 출력 배열 (NULL 이면 count 만 보고).
 * @array_size: domains 의 크기.
 * @return: 도메인 개수.
 *
 * 동기/배경: 상위 컨슈머 (예: NVMe-oF RDMA target) 가 어떤 메모리 도메인으로 데이터를
 * 전달해도 zero-copy 가 가능한지 미리 알기 위함. accel framework 의 generic 도메인 +
 * ENCRYPT 연산의 backend 별 도메인을 합쳐 보고.
 */
static int
vbdev_crypto_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct spdk_memory_domain **accel_domains = NULL;                        /* [한국어] accel ENCRYPT 도메인을 채울 출력 배열 시작 위치 (slot 부족 시 NULL → count-only). */
	int num_domains = 0, accel_rc, accel_array_size = 0;                     /* [한국어] num_domains=누적 도메인 수, accel_rc=accel 질의 결과, accel_array_size=accel 에 줄 슬롯 수. */

	/* Report generic accel and encryption module's memory domains */
	if (domains && num_domains < array_size) {                               /* [한국어] 출력 슬롯이 남아 있으면 첫 슬롯에 accel generic 도메인 기록. */
		domains[num_domains] = spdk_accel_get_memory_domain();           /* [한국어] accel framework 의 공용 host 메모리 도메인. */
	}

	num_domains++;                                                           /* [한국어] count 는 항상 증가 (NULL 호출이어도 size 계산용). */
	if (domains && num_domains < array_size) {                               /* [한국어] 남은 슬롯이 있으면 그 위치부터 accel 의 ENCRYPT 도메인 채우기. */
		accel_domains = domains + num_domains;                           /* [한국어] generic 도메인 다음 위치를 accel 출력 시작점으로. */
		accel_array_size = array_size - num_domains;                     /* [한국어] generic 1개를 뺀 나머지 슬롯 수. */
	}
	accel_rc = spdk_accel_get_opc_memory_domains(SPDK_ACCEL_OPC_ENCRYPT, accel_domains,
			accel_array_size);                                       /* [한국어] ENCRYPT 연산의 백엔드 별 도메인 list. */
	if (accel_rc > 0) {                                                      /* [한국어] accel 이 보고한 도메인이 있으면 누적. */
		num_domains += accel_rc;                                         /* [한국어] 총 도메인 수에 accel 도메인 수 합산. */
	}

	return num_domains;                                                      /* [한국어] 총 도메인 수 반환 (array_size 보다 크면 호출자가 재질의). */
}

/*
 * [한국어]
 * vbdev_crypto_sequence_supported - fn_table.accel_sequence_supported — READ/WRITE 만 sequence 지원.
 *
 * 동기/배경: 본 vbdev 는 사용자가 만든 accel_sequence 를 READ/WRITE 에서만 인계 가능.
 * UNMAP/FLUSH/RESET 등은 sequence 와 호환되지 않으므로 false 반환.
 */
static bool
vbdev_crypto_sequence_supported(void *ctx, enum spdk_bdev_io_type type)
{
	switch (type) {                                                          /* [한국어] I/O 타입별 sequence 인계 가능 여부 분기. */
	case SPDK_BDEV_IO_TYPE_READ:                                             /* [한국어] READ: decrypt sequence 인계 가능. */
	case SPDK_BDEV_IO_TYPE_WRITE:                                            /* [한국어] WRITE: encrypt sequence 인계 가능. */
		return true;                                                     /* [한국어] R/W 만 상위 accel_sequence 를 이어받아 처리. */
	default:                                                                 /* [한국어] UNMAP/FLUSH/RESET 등은 sequence 비호환. */
		return false;                                                    /* [한국어] sequence 인계 불가 보고 → 상위가 sequence 를 따로 처리. */
	}
}

/* When we register our bdev this is how we specify our entry points. */
/* [한국어] crypto vbdev 인스턴스의 fn_table — spdk_bdev_register 시 lib/bdev 가 호출하는 콜백 모음.
 * destruct: 리소스 해제 / submit_request: I/O 디스패처 / io_type_supported: 지원 I/O 종류 /
 * get_io_channel: per-thread 채널 발급 / dump_info_json: bdev_get_bdevs 응답 보강 /
 * get_memory_domains: zero-copy 가능 도메인 목록 / accel_sequence_supported: sequence 인계 허용 여부. */
static const struct spdk_bdev_fn_table vbdev_crypto_fn_table = {
	.destruct			= vbdev_crypto_destruct,                 /* [한국어] async destruct 트리거. */
	.submit_request			= vbdev_crypto_submit_request,           /* [한국어] R/W/UNMAP/FLUSH/RESET 디스패처. */
	.io_type_supported		= vbdev_crypto_io_type_supported,        /* [한국어] WZ 만 false, 나머지는 base 위임. */
	.get_io_channel			= vbdev_crypto_get_io_channel,           /* [한국어] per-thread crypto_io_channel 발급. */
	.dump_info_json			= vbdev_crypto_dump_info_json,           /* [한국어] bdev_get_bdevs 에 base/name/key_name 첨부. */
	.get_memory_domains		= vbdev_crypto_get_memory_domains,       /* [한국어] zero-copy 가능 메모리 도메인 보고. */
	.accel_sequence_supported	= vbdev_crypto_sequence_supported,       /* [한국어] R/W 만 sequence 인계 가능. */
};

/* [한국어] crypto bdev module 디스크립터 — SPDK_BDEV_MODULE_REGISTER 매크로로 lib/bdev 에 등록.
 * name: 모듈 이름 / module_init/fini: lifecycle / get_ctx_size: bdev_io 의 driver_ctx 영역 크기 /
 * examine_config: 모든 bdev 등장 시 호출 — base bdev 매칭 / config_json: save_config 시 RPC 직렬화. */
static struct spdk_bdev_module crypto_if = {
	.name = "crypto",                                                       /* [한국어] 모듈 식별 이름 — claim/unregister_by_name 에서 모듈 ID 로 사용. */
	.module_init = vbdev_crypto_init,                                       /* [한국어] 모듈 초기화 (현재는 no-op, 0 반환). */
	.get_ctx_size = vbdev_crypto_get_ctx_size,                              /* [한국어] bdev_io 의 driver_ctx 영역 크기 = sizeof(crypto_bdev_io). */
	.examine_config = vbdev_crypto_examine,                                 /* [한국어] 모든 bdev 등장 시 호출 — base 매칭하면 vbdev 생성. */
	.module_fini = vbdev_crypto_finish,                                     /* [한국어] 모듈 종료 — 남은 association 정리. */
	.config_json = vbdev_crypto_config_json                                 /* [한국어] save_config 시 bdev_crypto_create RPC 시퀀스 직렬화. */
};

SPDK_BDEV_MODULE_REGISTER(crypto, &crypto_if)                                  /* [한국어] constructor priority 로 lib/bdev 의 g_bdev_mgr 에 등록. */

/*
 * [한국어]
 * vbdev_crypto_claim - bdev_name 과 매칭되는 association 이 있으면 실제 vbdev 인스턴스 생성.
 *
 * @bdev_name: 등장한 base bdev 의 이름.
 * @return: 0=성공 또는 매칭 없음 (no-op), -ENODEV=base bdev 미존재, 그 외=실패.
 *
 * 동기/배경: examine_config 단계에서 등장한 모든 bdev 에 대해 호출된다. g_bdev_names
 * 의 각 항목과 매칭되는지 검사하고, 매칭되면 base bdev open / vbdev 구성 / UUID 생성 /
 * io_device 등록 / claim / register 의 풀 시퀀스를 수행한다. 매칭 실패 시 단순 return.
 *
 * 단계:
 *   1) UUID 네임스페이스 파싱.
 *   2) iobuf large_bufsize 조회 → max_rw_size 제한 계산.
 *   3) association 리스트 순회, 매칭되는 첫 항목에서:
 *      a) vbdev 구조체 할당, product_name 설정.
 *      b) base bdev open_ext + base_bdev 캐싱.
 *      c) base 의 properties 복사 (write_cache, optimal_io_boundary, blocklen, blockcnt).
 *      d) required_alignment = max(base, accel 의 ENCRYPT/DECRYPT 정렬 요구사항).
 *      e) fn_table / module / ctxt 연결.
 *      f) UUID = SHA1(ns_uuid, base_bdev->uuid).
 *      g) g_vbdev_crypto 등록, io_device register, thread 기록.
 *      h) module_claim_bdev 으로 소유권 주장, spdk_bdev_register.
 *   에러 시 goto 로 cleanup.
 *
 * 호출 체인: examine_config → vbdev_crypto_examine → 본 함수
 *           또는 create_crypto_disk → 본 함수.
 */
static int
vbdev_crypto_claim(const char *bdev_name)
{
	struct bdev_names *name;
	struct vbdev_crypto *vbdev;
	struct spdk_bdev *bdev;
	struct spdk_iobuf_opts iobuf_opts;
	struct spdk_accel_operation_exec_ctx opctx = {};                         /* [한국어] accel 의 alignment 질의에 전달할 ctx (block_size 만 채움). */
	struct spdk_uuid ns_uuid;
	int rc = 0;

	spdk_uuid_parse(&ns_uuid, BDEV_CRYPTO_NAMESPACE_UUID);                   /* [한국어] crypto 모듈의 UUID 네임스페이스 (상단 매크로) 파싱. */

	/* Limit the max IO size by some reasonable value. Since in write operation we use aux buffer,
	 * let's set the limit to the large_bufsize value */
	spdk_iobuf_get_opts(&iobuf_opts, sizeof(iobuf_opts));                    /* [한국어] iobuf 모듈의 옵션 (large_bufsize 등) 조회. */

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the crypto_bdev & bdev accordingly.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {                               /* [한국어] association 리스트 순회. */
		if (strcmp(name->opts->bdev_name, bdev_name) != 0) {             /* [한국어] base_bdev 이름 mismatch — 다음 항목. */
			continue;
		}
		SPDK_DEBUGLOG(vbdev_crypto, "Match on %s\n", bdev_name);

		vbdev = calloc(1, sizeof(struct vbdev_crypto));                  /* [한국어] vbdev 인스턴스 할당. */
		if (!vbdev) {
			SPDK_ERRLOG("Failed to allocate memory for crypto_bdev.\n");
			return -ENOMEM;
		}
		vbdev->crypto_bdev.product_name = "crypto";                      /* [한국어] bdev_get_bdevs 에서 product_name 으로 노출. */

		vbdev->crypto_bdev.name = strdup(name->opts->vbdev_name);        /* [한국어] vbdev 이름 (소유권 본 구조체). */
		if (!vbdev->crypto_bdev.name) {
			SPDK_ERRLOG("Failed to allocate memory for crypto_bdev name.\n");
			rc = -ENOMEM;
			goto error_bdev_name;
		}

		rc = spdk_bdev_open_ext(bdev_name, true, vbdev_crypto_base_bdev_event_cb,
					NULL, &vbdev->base_desc);                /* [한국어] base bdev open (write=true, event 콜백 등록). */
		if (rc) {
			if (rc != -ENODEV) {                                     /* [한국어] -ENODEV 는 정상 — deferred case 로 처리됨. */
				SPDK_ERRLOG("Failed to open bdev %s: error %d\n", bdev_name, rc);
			}
			goto error_open;
		}

		bdev = spdk_bdev_desc_get_bdev(vbdev->base_desc);                /* [한국어] desc → bdev 역참조. */
		vbdev->base_bdev = bdev;                                         /* [한국어] 본 vbdev 의 base_bdev 슬롯에 보관. */

		vbdev->crypto_bdev.write_cache = bdev->write_cache;              /* [한국어] write cache 능력 유지. */
		vbdev->crypto_bdev.optimal_io_boundary = bdev->optimal_io_boundary;
		vbdev->crypto_bdev.max_rw_size = spdk_min(
				bdev->max_rw_size ? bdev->max_rw_size : UINT32_MAX,
				iobuf_opts.large_bufsize / bdev->blocklen); /* [한국어] aux buffer 가 large_bufsize 한도이므로 그 안에 들어가는 LBA 수로 제한. */

		opctx.size = SPDK_SIZEOF(&opctx, block_size);                    /* [한국어] ctx 구조체의 valid 영역 표시 (ABI 호환성). */
		opctx.block_size = bdev->blocklen;                               /* [한국어] AES-XTS block_size 인자. */
		vbdev->crypto_bdev.required_alignment =
			spdk_max(bdev->required_alignment,
				 spdk_max(spdk_accel_get_buf_align(SPDK_ACCEL_OPC_ENCRYPT, &opctx),
					  spdk_accel_get_buf_align(SPDK_ACCEL_OPC_DECRYPT, &opctx))); /* [한국어] base + accel 양쪽 정렬 요구의 최댓값으로 통합. */

		vbdev->crypto_bdev.blocklen = bdev->blocklen;                    /* [한국어] crypto 는 1:1 매핑이므로 동일. */
		vbdev->crypto_bdev.blockcnt = bdev->blockcnt;

		/* This is the context that is passed to us when the bdev
		 * layer calls in so we'll save our crypto_bdev node here.
		 */
		vbdev->crypto_bdev.ctxt = vbdev;                                 /* [한국어] fn_table 콜백 ctx 로 vbdev 본체 전달. */
		vbdev->crypto_bdev.fn_table = &vbdev_crypto_fn_table;
		vbdev->crypto_bdev.module = &crypto_if;

		vbdev->crypto_bdev.numa = bdev->numa;                            /* [한국어] NUMA 노드 정보 전파 (alloc 정책 hint). */

		/* Assign crypto opts from the name. The pointer is valid up to the point
		 * the module is unloaded and all names removed from the list. */
		vbdev->opts = name->opts;                                        /* [한국어] opts 포인터 차용 (소유권은 여전히 name 에). */

		/* Generate UUID based on namespace UUID + base bdev UUID */
		rc = spdk_uuid_generate_sha1(&vbdev->crypto_bdev.uuid, &ns_uuid,
					     (const char *)&vbdev->base_bdev->uuid, sizeof(struct spdk_uuid)); /* [한국어] RFC 4122 §4.3: SHA1(ns, name). */
		if (rc) {
			SPDK_ERRLOG("Unable to generate new UUID for crypto bdev\n");
			goto error_uuid;
		}

		TAILQ_INSERT_TAIL(&g_vbdev_crypto, vbdev, link);                 /* [한국어] 활성 vbdev 리스트에 등록. */

		spdk_io_device_register(vbdev, crypto_bdev_ch_create_cb, crypto_bdev_ch_destroy_cb,
					sizeof(struct crypto_io_channel), vbdev->crypto_bdev.name); /* [한국어] per-thread 채널 관리용 io_device 등록 — vbdev 가 key. */

		/* Save the thread where the base device is opened */
		vbdev->thread = spdk_get_thread();                                /* [한국어] open thread 기록 — destruct 시 cross-thread close 분기에 사용. */

		rc = spdk_bdev_module_claim_bdev(bdev, vbdev->base_desc, vbdev->crypto_bdev.module); /* [한국어] base bdev 의 단독 소유권 주장 (다른 모듈 차단). */
		if (rc) {
			SPDK_ERRLOG("Failed to claim bdev %s\n", spdk_bdev_get_name(bdev));
			goto error_claim;
		}

		rc = spdk_bdev_register(&vbdev->crypto_bdev);                    /* [한국어] crypto vbdev 를 일반 bdev 로 등록 — 상위에 노출됨. */
		if (rc < 0) {
			SPDK_ERRLOG("Failed to register vbdev: error %d\n", rc);
			rc = -EINVAL;
			goto error_bdev_register;
		}
		SPDK_DEBUGLOG(vbdev_crypto, "Registered io_device and virtual bdev for: %s\n",
			      vbdev->opts->vbdev_name);
		break;                                                            /* [한국어] 매칭은 unique 하므로 한 개만 처리. */
	}

	return rc;

	/* Error cleanup paths. */
	/* [한국어] 단계별 cleanup label — 각 label 은 직전 단계까지 성공한 자원을 모두 해제한다. */
error_bdev_register:
	spdk_bdev_module_release_bdev(vbdev->base_bdev);                         /* [한국어] register 실패 시 claim 까지 했으므로 release. */
error_claim:
	TAILQ_REMOVE(&g_vbdev_crypto, vbdev, link);                              /* [한국어] 활성 리스트에서 제거. */
	spdk_io_device_unregister(vbdev, NULL);                                  /* [한국어] io_device 동기 unregister (NULL = no cb). */
error_uuid:
	spdk_bdev_close(vbdev->base_desc);                                       /* [한국어] base desc 해제. */
error_open:
	free(vbdev->crypto_bdev.name);
error_bdev_name:
	free(vbdev);

	return rc;
}

/* [한국어] crypto vbdev 비동기 삭제 컨텍스트 — unregister 콜백까지 전달된다. */
struct crypto_delete_disk_ctx {
	spdk_delete_crypto_complete cb_fn;
	/* [한국어] RPC 또는 외부 호출자에게 결과를 통지할 콜백.
	 * 설정자: delete_crypto_disk() 진입 시 호출자 인자 보관. */

	void *cb_arg;
	/* [한국어] cb_fn 첫 인자 (보통 RPC request 포인터). */

	char *bdev_name;
	/* [한국어] 삭제 대상 vbdev 이름 (호출자 owned 문자열의 복사본). */
};

/*
 * [한국어]
 * delete_crypto_disk_bdev_name - spdk_bdev_unregister_by_name 의 완료 콜백.
 *
 * @ctx: crypto_delete_disk_ctx 포인터.
 * @rc: unregister 결과 (0=성공, <0=실패).
 *
 * 동기/배경: unregister 가 끝나면 g_bdev_names 의 항목도 정리해 같은 base bdev 가
 * 나중에 다시 등장해도 자동 재생성되지 않도록 한다. 그 뒤 사용자 콜백 호출.
 */
static void
delete_crypto_disk_bdev_name(void *ctx, int rc)
{
	struct bdev_names *name;
	struct crypto_delete_disk_ctx *disk_ctx = ctx;

	/* Remove the association (vbdev, bdev) from g_bdev_names. This is required so that the
	 * vbdev does not get re-created if the same bdev is constructed at some other time,
	 * unless the underlying bdev was hot-removed. */
	TAILQ_FOREACH(name, &g_bdev_names, link) {                               /* [한국어] vbdev_name 으로 검색. */
		if (strcmp(name->opts->vbdev_name, disk_ctx->bdev_name) == 0) {
			vbdev_crypto_delete_name(name);                          /* [한국어] association + opts + 소유 시 DEK 해제. */
			break;
		}
	}

	disk_ctx->cb_fn(disk_ctx->cb_arg, rc);                                   /* [한국어] 최종 결과 보고. */

	free(disk_ctx->bdev_name);
	free(disk_ctx);
}

/* RPC entry for deleting a crypto vbdev. */
/*
 * [한국어]
 * delete_crypto_disk - 외부(RPC) 진입점: 이름으로 crypto vbdev 비동기 삭제.
 *
 * @bdev_name: 삭제할 vbdev 이름.
 * @cb_fn / @cb_arg: 완료 통지.
 *
 * 동기/배경: spdk_bdev_unregister_by_name 은 비동기로 unregister 후 cb 를 부른다.
 * 본 함수는 cb 전달을 위한 ctx 를 할당하고 unregister 를 트리거한다.
 */
void
delete_crypto_disk(const char *bdev_name, spdk_delete_crypto_complete cb_fn,
		   void *cb_arg)
{
	int rc;
	struct crypto_delete_disk_ctx *ctx;

	ctx = calloc(1, sizeof(struct crypto_delete_disk_ctx));                  /* [한국어] 비동기 ctx 할당. */
	if (!ctx) {
		SPDK_ERRLOG("Failed to allocate delete crypto disk ctx\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bdev_name = strdup(bdev_name);                                      /* [한국어] 호출자 문자열 복사 (lifetime 보장). */
	if (!ctx->bdev_name) {
		SPDK_ERRLOG("Failed to copy bdev_name\n");
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	ctx->cb_arg = cb_arg;
	ctx->cb_fn = cb_fn;
	/* Some cleanup happens in the destruct callback. */
	rc = spdk_bdev_unregister_by_name(bdev_name, &crypto_if, delete_crypto_disk_bdev_name, ctx); /* [한국어] 모듈 ID 와 함께 unregister 요청 — 다른 모듈의 동명 bdev 와 혼동 방지. */
	if (rc != 0) {                                                           /* [한국어] 동기 실패 (예: 존재하지 않음). */
		SPDK_ERRLOG("Encountered an error during bdev unregistration\n");
		cb_fn(cb_arg, rc);
		free(ctx->bdev_name);
		free(ctx);
	}
}

/* Because we specified this function in our crypto bdev function table when we
 * registered our crypto bdev, we'll get this call anytime a new bdev shows up.
 * Here we need to decide if we care about it and if so what to do. We
 * parsed the config file at init so we check the new bdev against the list
 * we built up at that time and if the user configured us to attach to this
 * bdev, here's where we do it.
 */
/*
 * [한국어]
 * vbdev_crypto_examine - module.examine_config — 새 bdev 가 등장할 때마다 lib/bdev 가 호출.
 *
 * @bdev: 등장한 base bdev.
 *
 * 동기/배경: 본 vbdev 모듈은 examine_config 단계 (vs examine_disk) 를 사용 —
 * 디스크 내용을 읽지 않고 단순히 이름 매칭만 하기 때문에 examine_config 가 적합.
 * 매칭되면 vbdev 인스턴스 생성, 어쨌든 examine_done 으로 다음 모듈에 진행 허용.
 */
static void
vbdev_crypto_examine(struct spdk_bdev *bdev)
{
	vbdev_crypto_claim(spdk_bdev_get_name(bdev));                            /* [한국어] 매칭 시 vbdev 생성. */
	spdk_bdev_module_examine_done(&crypto_if);                               /* [한국어] examine 완료 통지 — 다음 모듈로 진행. */
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_crypto)                                      /* [한국어] SPDK_DEBUGLOG(vbdev_crypto, …) 로 토글 가능한 log component 등록. */
