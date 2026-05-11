/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * [한국어 설명] mlx5 inline AES-XTS 스토리지 암호화 오프로드 구현 (mlx5_crypto.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVIDIA ConnectX-6 Dx 이상 NIC의 *inline crypto* 엔진을 SPDK에서 활용하기 위한
 * 컨트롤-플레인 구현이다. 구체적으로:
 *   (1) 시스템에 장착된 RDMA 디바이스 중 AES-XTS 가속을 지원하는 ConnectX 카드를 탐색·선별,
 *   (2) 디바이스 capability(crypto/crc32c/single_block_le_tweak/multi_block_le|be_tweak/
 *       wrapped_import_method 등)를 PRM(Programmer's Reference Manual)에 정의된 query_hca_cap
 *       명령(devx)으로 조회,
 *   (3) 사용자가 제공한 평문 DEK(Data Encryption Key, 128b 또는 256b key1+key2 ± 64b keytag)를
 *       각 디바이스에 등록하여 NIC 내부 key table의 obj_id를 받아오고,
 *   (4) 호출자가 PD로부터 DEK를 조회해 inline crypto MKEY를 등록하는 데 필요한 메타데이터
 *       (dek_obj_id, tweak_mode)를 제공.
 * 데이터-플레인(MKEY 작성, BSF segment, UMR WQE 등)은 mlx5_umr.c에서 별도로 처리하며 본 파일은
 * 컨트롤-플레인에 집중한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   bdev_crypto / accel_mlx5 → spdk_mlx5_crypto_keytag_create() →
 *     spdk_mlx5_crypto_devs_get() → spdk_mlx5_device_query_caps() (per-device) →
 *     mlx5_crypto_dek_init() (DEVX general object 생성) →
 *     mlx5_crypto_dek_query() (READY 상태 검증)
 *   런타임에 데이터-플레인이 spdk_mlx5_crypto_get_dek_data(keytag, pd)로 dek_obj_id/tweak_mode를
 *   읽어 BSF에 채워 보내면 NIC이 호스트→NVMe 또는 RDMA 송수신 path에서 자동으로 AES-XTS
 *   암복호화 수행. SPDK 측에서는 평문 데이터를 복사할 필요 없이 zero-copy 가속 가능.
 * 실행 컨텍스트: 컨트롤 경로(생성/파괴)는 init/teardown 시점에 메인 스레드에서 호출.
 * 데이터 경로의 spdk_mlx5_crypto_get_dek_data만 reactor 핫 패스에서 호출.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: librdmacm(rdma_get_devices), libibverbs(ibv_query_device/port), libmlx5dv(devx 명령),
 *   spdk_internal/rdma_utils.h(spdk_rdma_utils_get_pd / put_pd로 PD 캐싱),
 *   mlx5_ifc.h(PRM 자동생성 비트 정의), spdk_internal/mlx5.h(공개 wrapper API).
 * - 데이터 흐름: 사용자가 생성한 평문 DEK → mlx5_crypto_dek_init → DEVX_OBJ create →
 *   NIC firmware가 key table에 등록(key obj_id 반환) → 평문은 호스트 메모리에서 spdk_memset_s로 즉시 소거.
 *   조회 시: PD → linear search로 디바이스별 dek 매칭 → dek_obj_id/tweak_mode 반환.
 * - 공유 상태: 전역 g_allowed_devices 화이트리스트(파일 정적). spdk_mlx5_crypto_keytag는 호출자
 *   소유 객체로, 내부 deks[]는 디바이스 수만큼 할당됨.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct mlx5_crypto_dek_init_attr: DEK 생성 시 NIC에 보낼 입력 파라미터.
 * - struct mlx5_crypto_dek_query_attr: DEK 상태 조회 출력(state/opaque).
 * - struct mlx5_crypto_dek: 한 디바이스에 등록된 DEK 객체(devx_obj + obj_id + pd + tweak_mode).
 * - struct spdk_mlx5_crypto_keytag: 여러 디바이스에 걸친 DEK 묶음 + keytag(8B).
 * - mlx5_crypto_dev_allowed(): allow-list 검사.
 * - spdk_mlx5_crypto_devs_allow(): 사용자가 허용 디바이스 이름을 등록.
 * - spdk_mlx5_crypto_devs_get(): crypto-capable mlx5 디바이스 목록 반환.
 * - spdk_mlx5_device_query_caps(): query_hca_cap (general + crypto)으로 capability 조회.
 * - mlx5_crypto_dek_init(): create_encryption_key_obj DEVX 명령으로 키 등록.
 * - mlx5_crypto_dek_query(): query_general_object DEVX 명령으로 키 상태 조회.
 * - spdk_mlx5_crypto_keytag_create(): 모든 디바이스에 DEK를 등록한 keytag 생성(공개 API).
 * - spdk_mlx5_crypto_keytag_destroy(): keytag 파괴, DEK obj 제거, PD put, keytag 메모리 0으로 소거.
 * - spdk_mlx5_crypto_get_dek_data(): PD로 DEK 조회(런타임).
 */

/* [한국어] RDMA CM(Connection Manager) — rdma_get_devices/rdma_free_devices 제공.
 * 시스템의 모든 RDMA 디바이스 ibv_context를 한 번에 반환. */
#include <rdma/rdma_cma.h>
/* [한국어] 표준 libibverbs — ibv_query_device, ibv_query_port, ibv_pd 등 RDMA verbs 추상화. */
#include <infiniband/verbs.h>
/* [한국어] Mellanox direct-verbs — devx(direct experimental) 명령 인터페이스 mlx5dv_devx_*. */
#include <infiniband/mlx5dv.h>

/* [한국어] SPDK 표준 C 추상화. */
#include "spdk/stdinc.h"
/* [한국어] BSD queue/list 매크로. */
#include "spdk/queue.h"
/* [한국어] SPDK_ERRLOG / WARNLOG / DEBUGLOG. */
#include "spdk/log.h"
/* [한국어] spdk_likely / spdk_unlikely. */
#include "spdk/likely.h"
/* [한국어] SPDK_COUNTOF, spdk_memset_s 등 — 평문 키 보안 소거에 spdk_memset_s 사용(컴파일러 최적화로
 * 사라지지 않도록 보장된 안전 memset). */
#include "spdk/util.h"
/* [한국어] SPDK 내부 mlx5 wrapper API — spdk_mlx5_crypto_keytag, spdk_mlx5_device_caps 등 정의. */
#include "spdk_internal/mlx5.h"
/* [한국어] PD 캐싱 유틸 (spdk_rdma_utils_get_pd/put_pd) — 동일 ibv_context의 PD를 재사용. */
#include "spdk_internal/rdma_utils.h"
/* [한국어] PRM 자동생성 정의 — DEVX_SET/GET, MLX5_CMD_OP_*, MLX5_OBJ_TYPE_DEK 등. */
#include "mlx5_ifc.h"
/* [한국어] lib/mlx5 내부 정의 — 본 파일은 직접 사용하진 않으나 의존성 유지. */
#include "mlx5_priv.h"

/* Plaintext key sizes */
/* 64b keytag */
/* [한국어] AES-XTS keytag(8B). 사용자가 제공한 raw key buffer 끝에 선택적으로 따라붙는 64비트
 * 무결성 태그. NIC이 key 사용 시점에 비교하여 키 자체의 변조/오인 사용을 탐지. */
#define SPDK_MLX5_AES_XTS_KEYTAG_SIZE 8
/* key1_128b + key2_128b */
/* [한국어] AES-XTS-128 모드 평문 DEK 크기 = key1(16B) + key2(16B) = 32B. */
#define SPDK_MLX5_AES_XTS_128_DEK_BYTES 32
/* key1_256b + key2_256b */
/* [한국어] AES-XTS-256 모드 평문 DEK 크기 = key1(32B) + key2(32B) = 64B. */
#define SPDK_MLX5_AES_XTS_256_DEK_BYTES 64
/* key1_128b + key2_128b + 64b_keytag */
/* [한국어] 128b 모드 + keytag 포함 시 = 32 + 8 = 40B. */
#define SPDK_MLX5_AES_XTS_128_DEK_BYTES_WITH_KEYTAG (SPDK_MLX5_AES_XTS_128_DEK_BYTES + SPDK_MLX5_AES_XTS_KEYTAG_SIZE)
/* key1_256b + key2_256b + 64b_keytag */
/* [한국어] 256b 모드 + keytag 포함 시 = 64 + 8 = 72B. */
#define SPDK_MLX5_AES_XTS_256_DEK_BYTES_WITH_KEYTAG (SPDK_MLX5_AES_XTS_256_DEK_BYTES + SPDK_MLX5_AES_XTS_KEYTAG_SIZE)

/*
 * [한국어]
 * struct mlx5_crypto_dek_init_attr - DEK 생성용 입력 파라미터.
 *
 * mlx5_crypto_dek_init이 PRM의 create_encryption_key_obj 명령에 채워 보낸다.
 */
struct mlx5_crypto_dek_init_attr {
	char *dek;
	/* [한국어] 평문 키 raw 버퍼 포인터. 호출자(보통 keytag_create) 소유.
	 * 설정자: spdk_mlx5_crypto_keytag_create(). 읽는 자: mlx5_crypto_dek_init이 NIC에 등록 후 즉시 소거.
	 * 보안: 등록 후 spdk_memset_s로 NIC 명령 버퍼 영역 평문을 소거 — 호스트 메모리에 평문 흔적 최소화. */

	uint64_t opaque;
	/* [한국어] 사용자 정의 64비트 마커. NIC은 그대로 보관했다가 query 시 반환 — 디버그/감사용.
	 * 본 파일은 0으로 고정하고 query_attr.opaque == 0 검사로 일관성만 확인. */

	uint32_t key_size_bytes;
	/* [한국어] 평문 키 길이(바이트). 32/40/64/72 중 하나(위 매크로 참조). */

	uint8_t key_size; /* driver representation of \b key_size_bytes */
	/* [한국어] PRM 인코딩 형식의 키 크기 — MLX5_ENCRYPTION_KEY_OBJ_KEY_SIZE_SIZE_128 또는 _256.
	 * key_size_bytes에서 변환된 값. */

	uint8_t keytag;
	/* [한국어] 1이면 keytag 사용, 0이면 미사용. NIC encryption_key_obj.has_keytag 비트로 매핑. */
};

/*
 * [한국어]
 * struct mlx5_crypto_dek_query_attr - DEK 상태 조회 출력.
 */
struct mlx5_crypto_dek_query_attr {
	/* state either MLX5_ENCRYPTION_KEY_OBJ_STATE_READY or MLX5_ENCRYPTION_KEY_OBJ_STATE_ERROR */
	uint8_t state;
	/* [한국어] 키 객체 상태. READY면 사용 가능, ERROR면 NIC가 거부.
	 * 설정자: mlx5_crypto_dek_query()가 NIC 응답에서 추출. 읽는 자: keytag_create의 검증 로직. */

	uint64_t opaque;
	/* [한국어] 등록 시 보낸 opaque가 그대로 반환되어야 함(0이어야 함 — 본 파일 가정). */
};

/*
 * [한국어]
 * struct mlx5_crypto_dek - 디바이스 한 대에 등록된 DEK 객체.
 *
 * keytag 안의 deks[] 배열의 각 원소가 디바이스 하나에 대응. 동일 keytag의 모든 DEK는 같은
 * 평문 키로부터 생성됐으므로 상호 호환되는 ciphertext를 만든다.
 */
struct mlx5_crypto_dek {
	struct mlx5dv_devx_obj *devx_obj;
	/* [한국어] NIC에 생성된 general object 핸들. mlx5dv_devx_obj_destroy로 파괴.
	 * 설정자: mlx5_crypto_dek_init. 읽는 자: deinit, query, get_dek_data. */

	struct ibv_pd *pd;
	/* [한국어] 이 DEK가 바인딩된 Protection Domain. 같은 PD를 사용하는 MR과만 inline crypto MKEY로
	 * 결합 가능. spdk_rdma_utils_get_pd로 가져와 put_pd로 반환. 캐시 hit 시 재사용. */

	struct ibv_context *context;
	/* [한국어] 이 DEK가 등록된 RDMA 디바이스의 ibv_context. devs[i] 그대로 저장.
	 * 디바이스 식별 및 디버그 출력에 사용. */

	/* Cached dek_obj_id */
	uint32_t dek_obj_id;
	/* [한국어] NIC이 부여한 32비트 객체 ID. PRM의 create_encryption_key_obj 응답에서 추출.
	 * 데이터-플레인이 BSF segment의 dek_pointer 필드에 BE32로 박아 보내면 NIC이 key table에서
	 * 해당 키를 lookup하여 AES-XTS 수행. */

	enum spdk_mlx5_crypto_key_tweak_mode tweak_mode;
	/* [한국어] AES-XTS tweak(=initial vector with LBA) 인코딩 방식. SIMPLE_LBA_BE 또는 SIMPLE_LBA_LE.
	 * 디바이스 capability에 따라 BE 우선 선택(multi_block_be_tweak 지원 시).
	 * 데이터-플레인이 BSF의 xts_initial_tweak를 어떻게 채울지 결정. */
};

/*
 * [한국어]
 * struct spdk_mlx5_crypto_keytag - 여러 디바이스의 DEK를 묶은 사용자 핸들.
 *
 * 시스템에 crypto-capable mlx5 디바이스가 N개 있으면 같은 평문 키를 N개의 DEK로 등록해 두고,
 * 데이터-플레인에서 사용 중인 PD에 매칭되는 DEK를 골라 사용한다.
 */
struct spdk_mlx5_crypto_keytag {
	struct mlx5_crypto_dek *deks;
	/* [한국어] 디바이스별 DEK 배열(0 ~ deks_num-1). calloc 할당. */

	uint32_t deks_num;
	/* [한국어] 실제로 등록 성공한 DEK 수. 부분 실패 시 destroy가 이만큼만 정리. */

	bool has_keytag;
	/* [한국어] 평문 키에 keytag 8B가 포함됐는지 여부. true면 keytag[]에 복사 보관. */

	char keytag[8];
	/* [한국어] 64비트 keytag 보관. 데이터-플레인이 MKEY 등록 시 BSF의 keytag 필드에 채움.
	 * 파괴 시 spdk_memset_s로 0 소거. */
};

/* [한국어] 사용자 화이트리스트(NULL이면 모든 디바이스 허용). spdk_mlx5_crypto_devs_allow로 설정.
 * 동시성: init 시점에만 갱신되고 런타임 read-only로 가정 — 락 없음. */
static char **g_allowed_devices;
/* [한국어] g_allowed_devices 배열 크기. 0이면 화이트리스트 비활성. */
static size_t g_allowed_devices_count;

/*
 * [한국어]
 * mlx5_crypto_devs_free - 화이트리스트 메모리 해제.
 *
 * spdk_mlx5_crypto_devs_allow가 새 리스트를 등록하기 전 또는 모듈 정리 시 호출.
 */
static void
mlx5_crypto_devs_free(void)
{
	size_t i;

	if (!g_allowed_devices) {
		return;
		/* [한국어] 이미 비어있으면 no-op. */
	}

	for (i = 0; i < g_allowed_devices_count; i++) {
		free(g_allowed_devices[i]);
		/* [한국어] strndup으로 복제했던 이름 문자열 해제. */
	}
	free(g_allowed_devices);
	/* [한국어] 포인터 배열 자체 해제. */
	g_allowed_devices = NULL;
	g_allowed_devices_count = 0;
	/* [한국어] 전역 상태 리셋 — 다음에 다시 등록 가능. */
}

/*
 * [한국어]
 * mlx5_crypto_dev_allowed - 디바이스 이름이 화이트리스트에 있는지 검사.
 *
 * @dev: 검사할 디바이스 이름("mlx5_0" 등).
 * @return: 화이트리스트 비활성이거나 매칭 시 true, 매칭 없을 때 false.
 *
 * 호출 체인: spdk_mlx5_crypto_devs_get → [mlx5_crypto_dev_allowed]
 */
static bool
mlx5_crypto_dev_allowed(const char *dev)
{
	size_t i;

	if (!g_allowed_devices || !g_allowed_devices_count) {
		return true;
		/* [한국어] 화이트리스트 미설정 → 모두 허용. */
	}

	for (i = 0; i < g_allowed_devices_count; i++) {
		if (strcmp(g_allowed_devices[i], dev) == 0) {
			return true;
			/* [한국어] 정확히 일치하는 이름 발견 → 허용. */
		}
	}

	return false;
	/* [한국어] 매칭 실패 → 거부. */
}

/*
 * [한국어]
 * spdk_mlx5_crypto_devs_allow - 사용자 화이트리스트 설정 공개 API.
 *
 * @dev_names: 디바이스 이름 문자열 배열. NULL이거나 devs_count==0이면 화이트리스트 해제.
 * @devs_count: 배열 크기.
 * @return: 0 성공, -ENOMEM 메모리 실패.
 *
 * 기존 리스트는 free하고 새로 strndup으로 복제. 호출자가 dev_names의 lifetime을 신경쓰지 않게.
 */
int
spdk_mlx5_crypto_devs_allow(const char *const dev_names[], size_t devs_count)
{
	size_t i;

	mlx5_crypto_devs_free();
	/* [한국어] 기존 리스트 정리. */

	if (!dev_names || !devs_count) {
		return 0;
		/* [한국어] 인자가 비면 화이트리스트 해제 상태로 종료. */
	}

	g_allowed_devices = calloc(devs_count, sizeof(char *));
	if (!g_allowed_devices) {
		return -ENOMEM;
		/* [한국어] 포인터 배열 할당 실패. */
	}
	for (i = 0; i < devs_count; i++) {
		g_allowed_devices[i] = strndup(dev_names[i], SPDK_MLX5_DEV_MAX_NAME_LEN);
		/* [한국어] 이름 안전 복제. SPDK_MLX5_DEV_MAX_NAME_LEN으로 길이 제한 — buffer overrun 방어. */
		if (!g_allowed_devices[i]) {
			mlx5_crypto_devs_free();
			/* [한국어] 부분 실패 시 일관성 위해 전체 정리. */
			return -ENOMEM;
		}
		g_allowed_devices_count++;
		/* [한국어] 성공한 항목까지 카운트 증가 — free 시 정확한 범위 보장. */
	}

	return 0;
}

/*
 * [한국어]
 * spdk_mlx5_crypto_devs_get - crypto-capable mlx5 디바이스 ibv_context 배열 반환.
 *
 * @dev_num: 출력. 반환된 디바이스 수.
 * @return: ibv_context 포인터 배열(NULL-terminated, 호출자가 spdk_mlx5_crypto_devs_release로 해제).
 *
 * 동작:
 *   1. rdma_get_devices()로 모든 RDMA 디바이스 조회.
 *   2. 각 디바이스에 대해:
 *      - vendor_id == Mellanox 인지.
 *      - 화이트리스트 통과.
 *      - port의 link layer가 ETHERNET이면 RoCE enabled 여부 확인(query_nic_vport_context).
 *      - spdk_mlx5_device_query_caps로 crypto/AES-XTS 지원, wrapped_import_method 비활성 확인.
 *   3. 통과한 디바이스만 모아 반환.
 *
 * 호출 체인:
 *   spdk_mlx5_crypto_keytag_create → [spdk_mlx5_crypto_devs_get] → spdk_mlx5_device_query_caps
 */
struct ibv_context **
spdk_mlx5_crypto_devs_get(int *dev_num)
{
	struct ibv_context **rdma_devs, **rdma_devs_out = NULL, *dev;
	struct ibv_device_attr dev_attr;
	struct ibv_port_attr port_attr;
	struct spdk_mlx5_device_caps dev_caps;
	uint8_t in[DEVX_ST_SZ_BYTES(query_nic_vport_context_in)];
	uint8_t out[DEVX_ST_SZ_BYTES(query_nic_vport_context_out)];
	/* [한국어] DEVX 명령 입출력 버퍼 — PRM 자동생성 매크로로 정확한 크기 산출. */
	uint8_t devx_v;
	int num_rdma_devs = 0, i, rc;
	int num_crypto_devs = 0;

	/* query all devices, save mlx5 with crypto support */
	rdma_devs = rdma_get_devices(&num_rdma_devs);
	/* [한국어] librdmacm 호출 — 시스템의 모든 RDMA ibv_context 배열 반환. */
	if (!rdma_devs || !num_rdma_devs) {
		*dev_num = 0;
		return NULL;
		/* [한국어] RDMA 디바이스 자체가 없음. */
	}

	rdma_devs_out = calloc(num_rdma_devs + 1, sizeof(*rdma_devs_out));
	/* [한국어] +1: NULL terminator. 화이트리스트/cap 통과한 디바이스만 채움. */
	if (!rdma_devs_out) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}

	for (i = 0; i < num_rdma_devs; i++) {
		dev = rdma_devs[i];
		rc = ibv_query_device(dev, &dev_attr);
		/* [한국어] 디바이스 일반 속성(vendor_id 등) 조회 — 시스템 콜 1회. */
		if (rc) {
			SPDK_ERRLOG("Failed to query dev %s, skipping\n", dev->device->name);
			continue;
		}
		if (dev_attr.vendor_id != SPDK_MLX5_VENDOR_ID_MELLANOX) {
			/* [한국어] Mellanox PCI vendor ID(0x02c9)가 아닌 디바이스는 본 라이브러리 적용 대상 아님. */
			SPDK_DEBUGLOG(mlx5, "dev %s is not Mellanox device, skipping\n", dev->device->name);
			continue;
		}

		if (!mlx5_crypto_dev_allowed(dev->device->name)) {
			/* [한국어] 화이트리스트 차단. */
			continue;
		}

		rc = ibv_query_port(dev, 1, &port_attr);
		/* [한국어] 포트 1번(SPDK는 dual-port HCA에서 첫 번째 포트만 사용)의 속성 조회. */
		if (rc) {
			SPDK_ERRLOG("Failed to query port attributes for device %s, rc %d\n", dev->device->name, rc);
			continue;
		}

		if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
			/* Port may be ethernet but roce is still disabled */
			/* [한국어] ETH 링크인 경우 RoCE(RDMA over Converged Ethernet)가 켜져 있는지 추가 확인. */
			memset(in, 0, sizeof(in));
			memset(out, 0, sizeof(out));
			DEVX_SET(query_nic_vport_context_in, in, opcode, MLX5_CMD_OP_QUERY_NIC_VPORT_CONTEXT);
			/* [한국어] DEVX 명령 헤더 작성: opcode = QUERY_NIC_VPORT_CONTEXT. */
			rc = mlx5dv_devx_general_cmd(dev, in, sizeof(in), out, sizeof(out));
			/* [한국어] NIC firmware에 명령 전송 → in 버퍼 송신, out 버퍼에 응답 수신. */
			if (rc) {
				SPDK_ERRLOG("Failed to get VPORT context for device %s. Assuming ROCE is disabled\n",
					    dev->device->name);
				continue;
			}

			devx_v = DEVX_GET(query_nic_vport_context_out, out, nic_vport_context.roce_en);
			/* [한국어] 응답에서 roce_en 비트 추출. */
			if (!devx_v) {
				SPDK_ERRLOG("Device %s, RoCE disabled\n", dev->device->name);
				continue;
				/* [한국어] RoCE 꺼져있음 → 본 라이브러리는 RC QP 기반이라 사용 불가. */
			}
		}

		memset(&dev_caps, 0, sizeof(dev_caps));
		rc = spdk_mlx5_device_query_caps(dev, &dev_caps);
		/* [한국어] HCA cap 조회 — crypto/AES-XTS/wrapped_import_method 등. */
		if (rc) {
			SPDK_ERRLOG("Failed to query mlx5 dev %s, skipping\n", dev->device->name);
			continue;
		}
		if (!dev_caps.crypto_supported) {
			SPDK_WARNLOG("dev %s crypto engine doesn't support crypto\n", dev->device->name);
			continue;
			/* [한국어] crypto 엔진 자체가 없는 펌웨어/모델 — pass. */
		}
		if (!(dev_caps.crypto.single_block_le_tweak || dev_caps.crypto.multi_block_le_tweak ||
		      dev_caps.crypto.multi_block_be_tweak)) {
			SPDK_WARNLOG("dev %s crypto engine doesn't support AES_XTS\n", dev->device->name);
			continue;
			/* [한국어] AES-XTS의 어떤 tweak 모드도 지원 안 함 → 본 라이브러리 사용 불가. */
		}
		if (dev_caps.crypto.wrapped_import_method_aes_xts) {
			SPDK_WARNLOG("dev %s uses wrapped import method which is not supported by mlx5 lib\n",
				     dev->device->name);
			continue;
			/* [한국어] wrapped import 모드(KEK으로 키를 감싸 import)는 별도 KMS 통합 필요 — 미지원. */
		}

		rdma_devs_out[num_crypto_devs++] = dev;
		/* [한국어] 모든 검사 통과 — 출력 배열에 추가. */
	}

	if (!num_crypto_devs) {
		SPDK_DEBUGLOG(mlx5, "Found no mlx5 crypto devices\n");
		goto err_out;
	}

	rdma_free_devices(rdma_devs);
	/* [한국어] 원본 RDMA 디바이스 배열 해제. ibv_context 핸들은 여전히 유효(rdma_devs_out에 보관). */
	*dev_num = num_crypto_devs;

	return rdma_devs_out;

err_out:
	free(rdma_devs_out);
	rdma_free_devices(rdma_devs);
	*dev_num = 0;
	return NULL;
	/* [한국어] crypto-capable 0개 → 모든 자원 해제 후 NULL. */
}

/*
 * [한국어]
 * spdk_mlx5_crypto_devs_release - spdk_mlx5_crypto_devs_get가 반환한 배열을 해제.
 *
 * 호출자가 사용 끝난 뒤 호출. ibv_context 핸들 자체는 RDMA core가 소유하므로 free하지 않고
 * 배열 메모리만 해제.
 */
void
spdk_mlx5_crypto_devs_release(struct ibv_context **rdma_devs)
{
	if (rdma_devs) {
		free(rdma_devs);
		/* [한국어] calloc으로 할당된 포인터 배열만 해제. */
	}
}

/*
 * [한국어]
 * spdk_mlx5_device_query_caps - 디바이스 capability를 PRM query_hca_cap 명령으로 조회.
 *
 * @context: 대상 디바이스.
 * @caps: 출력 cap 구조체.
 * @return: 0 성공, devx 명령 실패 시 음수.
 *
 * 두 단계로 cap을 조회:
 *   1. opmod = GENERAL_DEVICE | GET_CUR — crc32c, crypto, AES_XTS tweak 지원 여부.
 *   2. opmod = CRYPTO | GET_CUR — wrapped_crypto_operational, going_to_commissioning,
 *      wrapped_import_method_aes_xts.
 * crypto_supported가 false면 2단계 생략.
 *
 * 호출 체인:
 *   spdk_mlx5_crypto_devs_get / spdk_mlx5_crypto_keytag_create → [spdk_mlx5_device_query_caps]
 */
int
spdk_mlx5_device_query_caps(struct ibv_context *context, struct spdk_mlx5_device_caps *caps)
{
	uint16_t opmod = MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE |
			 HCA_CAP_OPMOD_GET_CUR;
	/* [한국어] op_mod 조합: GENERAL_DEVICE 카테고리 + 현재 값 조회 (GET_CUR vs GET_MAX). */
	uint32_t out[DEVX_ST_SZ_DW(query_hca_cap_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(query_hca_cap_in)] = {};
	int rc;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	/* [한국어] 명령 헤더 opcode = QUERY_HCA_CAP. */
	DEVX_SET(query_hca_cap_in, in, op_mod, opmod);
	/* [한국어] op_mod로 어떤 cap을 조회할지 지정. */

	rc = mlx5dv_devx_general_cmd(context, in, sizeof(in), out, sizeof(out));
	/* [한국어] 동기 DEVX 명령 — NIC firmware가 응답을 out 버퍼에 채워줌. */
	if (rc) {
		return rc;
	}

	caps->crc32c_supported = DEVX_GET(query_hca_cap_out, out, capability.cmd_hca_cap.sho) &&
				 DEVX_GET(query_hca_cap_out, out, capability.cmd_hca_cap.sig_crc32c);
	/* [한국어] CRC32C signature offload 지원: 일반 sho 비트 AND CRC32C 전용 비트. */

	caps->crypto_supported = DEVX_GET(query_hca_cap_out, out, capability.cmd_hca_cap.crypto);
	/* [한국어] crypto 일반 지원 비트. */
	if (!caps->crypto_supported) {
		return 0;
		/* [한국어] crypto 미지원 → 추가 cap 조회 불필요, 0 반환. */
	}

	caps->crypto.single_block_le_tweak = DEVX_GET(query_hca_cap_out,
					     out, capability.cmd_hca_cap.aes_xts_single_block_le_tweak);
	/* [한국어] 단일 블록 LE tweak 지원 — 한 LBA = 하나의 16B AES 블록. */
	caps->crypto.multi_block_be_tweak = DEVX_GET(query_hca_cap_out, out,
					    capability.cmd_hca_cap.aes_xts_multi_block_be_tweak);
	/* [한국어] 다중 블록 BE tweak 지원 — 한 LBA가 여러 16B 블록일 때 BE 인코딩. */
	caps->crypto.multi_block_le_tweak = DEVX_GET(query_hca_cap_out, out,
					    capability.cmd_hca_cap.aes_xts_multi_block_le_tweak);
	/* [한국어] 다중 블록 LE tweak 지원. */

	opmod = MLX5_SET_HCA_CAP_OP_MOD_CRYPTO | HCA_CAP_OPMOD_GET_CUR;
	/* [한국어] 두 번째 조회: CRYPTO 카테고리. */
	memset(&out, 0, sizeof(out));
	memset(&in, 0, sizeof(in));

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, opmod);

	rc = mlx5dv_devx_general_cmd(context, in, sizeof(in), out, sizeof(out));
	if (rc) {
		return rc;
	}

	caps->crypto.wrapped_crypto_operational = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_caps.wrapped_crypto_operational);
	/* [한국어] wrapped crypto 모드 운영 가능 여부 — 본 라이브러리는 사용하지 않음. */
	caps->crypto.wrapped_crypto_going_to_commissioning = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_caps .wrapped_crypto_going_to_commissioning);
	/* [한국어] commissioning 진행 중 상태 — FIPS-like 인증 시퀀스 관련. */
	caps->crypto.wrapped_import_method_aes_xts = (DEVX_GET(query_hca_cap_out, out,
			capability.crypto_caps.wrapped_import_method) &
			MLX5_CRYPTO_CAPS_WRAPPED_IMPORT_METHOD_AES) != 0;
	/* [한국어] wrapped_import_method가 AES 비트를 포함하면 wrapped 모드 강제 — 본 라이브러리 사용 불가.
	 * 위 spdk_mlx5_crypto_devs_get가 이를 reject 사유로 사용. */

	return 0;
}

/*
 * [한국어]
 * mlx5_crypto_dek_deinit - DEK general object 파괴.
 *
 * 호출 체인: spdk_mlx5_crypto_keytag_destroy → [mlx5_crypto_dek_deinit]
 */
static void
mlx5_crypto_dek_deinit(struct mlx5_crypto_dek *dek)
{
	int rc;

	rc = mlx5dv_devx_obj_destroy(dek->devx_obj);
	/* [한국어] NIC key table에서 키 객체 제거. 평문 키는 init 시점에 이미 호스트 메모리에서 소거됨. */
	if (rc) {
		SPDK_ERRLOG("Failed to destroy crypto obj:%p, rc %d\n", dek->devx_obj, rc);
		/* [한국어] 파괴 실패는 메모리 누수가 아니라 NIC 자원 누수 — 운영 잡음으로 ERRLOG. */
	}
}

/*
 * [한국어]
 * spdk_mlx5_crypto_keytag_destroy - keytag 전체 파괴(공개 API).
 *
 * @keytag: 파괴할 keytag(NULL 허용).
 *
 * 동작:
 *   1. 모든 DEK에 대해 devx_obj 파괴 + PD put.
 *   2. keytag.keytag(8B 평문 보조 데이터)를 spdk_memset_s로 안전 소거.
 *   3. deks 배열 free, keytag 자체 free.
 */
void
spdk_mlx5_crypto_keytag_destroy(struct spdk_mlx5_crypto_keytag *keytag)
{
	struct mlx5_crypto_dek *dek;
	uint32_t i;

	if (!keytag) {
		return;
		/* [한국어] NULL guard. */
	}

	for (i = 0; i < keytag->deks_num; i++) {
		dek = &keytag->deks[i];
		if (dek->devx_obj) {
			mlx5_crypto_dek_deinit(dek);
			/* [한국어] NIC key table에서 제거. */
		}
		if (dek->pd) {
			spdk_rdma_utils_put_pd(dek->pd);
			/* [한국어] PD ref 감소(캐싱된 PD 공유). 마지막 사용자가 put하면 PD가 dealloc. */
		}
	}
	spdk_memset_s(keytag->keytag, sizeof(keytag->keytag), 0, sizeof(keytag->keytag));
	/* [한국어] keytag 8B 평문을 안전 소거 — 컴파일러가 dead-store로 제거하지 않도록 보장된 memset_s. */
	free(keytag->deks);
	free(keytag);
}

/*
 * [한국어]
 * mlx5_crypto_dek_init - 한 디바이스에 평문 DEK를 등록.
 *
 * @pd: 키와 바인딩될 PD.
 * @attr: 키 raw 데이터 + 사이즈 + keytag 여부.
 * @dek: 출력 DEK 객체(devx_obj, dek_obj_id 채워짐).
 * @return: 0 성공, 음수 errno.
 *
 * PRM의 create_encryption_key_obj 명령(general object create)을 발행. NIC은 평문 키를 받아
 * 내부 key table 슬롯에 보관하고 obj_id를 반환. 호스트는 응답 수신 즉시 명령 버퍼의 평문 영역을
 * spdk_memset_s로 0으로 소거.
 *
 * 호출 체인:
 *   spdk_mlx5_crypto_keytag_create → [mlx5_crypto_dek_init] → mlx5_get_pd_id, mlx5dv_devx_obj_create
 */
static int
mlx5_crypto_dek_init(struct ibv_pd *pd, struct mlx5_crypto_dek_init_attr *attr,
		     struct mlx5_crypto_dek *dek)
{
	uint32_t in[DEVX_ST_SZ_DW(create_encryption_key_obj_in)] = {};
	uint32_t out[DEVX_ST_SZ_DW(general_obj_out_cmd_hdr)] = {};
	/* [한국어] DEVX 명령 in/out 버퍼. in에는 헤더+key 데이터, out에는 헤더+obj_id. */
	uint8_t *dek_in;
	uint32_t pdn;
	int rc;

	rc = mlx5_get_pd_id(pd, &pdn);
	/* [한국어] mlx5dv_init_obj로 PD index(pdn) 추출. PRM 명령에 직접 박아 보내야 함. */
	if (rc) {
		return rc;
	}

	dek_in = DEVX_ADDR_OF(create_encryption_key_obj_in, in, hdr);
	/* [한국어] in 버퍼에서 hdr 필드의 시작 주소 — general_obj_in_cmd_hdr 양식. */
	DEVX_SET(general_obj_in_cmd_hdr, dek_in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	/* [한국어] opcode = CREATE_GENERAL_OBJECT. */
	DEVX_SET(general_obj_in_cmd_hdr, dek_in, obj_type, MLX5_OBJ_TYPE_DEK);
	/* [한국어] obj_type = DEK. NIC가 어떤 종류의 객체를 만들지 결정. */
	dek_in = DEVX_ADDR_OF(create_encryption_key_obj_in, in, key_obj);
	/* [한국어] in 버퍼에서 key_obj(=encryption_key_obj) 시작 주소로 이동. */
	DEVX_SET(encryption_key_obj, dek_in, key_size, attr->key_size);
	/* [한국어] PRM 인코딩 키 사이즈(SIZE_128 / SIZE_256). */
	DEVX_SET(encryption_key_obj, dek_in, has_keytag, attr->keytag);
	/* [한국어] keytag 사용 여부(0/1). */
	DEVX_SET(encryption_key_obj, dek_in, key_purpose, MLX5_ENCRYPTION_KEY_OBJ_KEY_PURPOSE_AES_XTS);
	/* [한국어] 키 용도 = AES-XTS. NIC이 다른 알고리즘으로 오용하지 못하게. */
	DEVX_SET(encryption_key_obj, dek_in, pd, pdn);
	/* [한국어] 키와 PD 바인딩. 동일 PD MR과만 함께 사용 가능. */
	memcpy(DEVX_ADDR_OF(encryption_key_obj, dek_in, opaque), &attr->opaque, sizeof(attr->opaque));
	/* [한국어] opaque 8B 복사 — query 시 그대로 회수되어 일관성 검증에 사용. */
	memcpy(DEVX_ADDR_OF(encryption_key_obj, dek_in, key), attr->dek, attr->key_size_bytes);
	/* [한국어] 평문 키 raw bytes 복사. 이 시점에 NIC 명령 버퍼에 평문이 존재 — 곧 소거됨. */

	dek->devx_obj = mlx5dv_devx_obj_create(pd->context, in, sizeof(in), out, sizeof(out));
	/* [한국어] DEVX general object 생성 — NIC firmware로 평문 키 전달. 동기 호출. */
	spdk_memset_s(DEVX_ADDR_OF(encryption_key_obj, dek_in, key), attr->key_size_bytes, 0,
		      attr->key_size_bytes);
	/* [한국어] 즉시 in 버퍼의 평문 키 영역을 안전 소거. spdk_memset_s는 dead-store 최적화 방어. */
	if (!dek->devx_obj) {
		return -errno;
		/* [한국어] 명령 실패 — verbs/devx가 errno 설정. */
	}
	dek->dek_obj_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);
	/* [한국어] NIC이 부여한 32비트 객체 ID 추출 — 데이터-플레인이 BSF.dek_pointer로 사용. */

	return 0;
}

/*
 * [한국어]
 * mlx5_crypto_dek_query - DEK 객체의 현재 상태 조회.
 *
 * @dek: 조회 대상.
 * @attr: 출력(state, opaque).
 * @return: 0 성공, 음수 errno.
 *
 * PRM query_general_object 명령으로 obj_id가 가리키는 DEK의 state(READY/ERROR)와 opaque를 회수.
 * keytag_create가 등록 직후 호출하여 NIC이 키를 정상 수용했는지 확인.
 *
 * 호출 체인:
 *   spdk_mlx5_crypto_keytag_create → [mlx5_crypto_dek_query]
 */
static int
mlx5_crypto_dek_query(struct mlx5_crypto_dek *dek, struct mlx5_crypto_dek_query_attr *attr)
{
	uint32_t out[DEVX_ST_SZ_DW(query_encryption_key_obj_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(general_obj_in_cmd_hdr)] = {};
	uint8_t *dek_out;
	int rc;

	assert(attr);
	DEVX_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_QUERY_GENERAL_OBJECT);
	/* [한국어] opcode = QUERY_GENERAL_OBJECT. */
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_OBJ_TYPE_DEK);
	/* [한국어] 조회할 객체 타입 = DEK. */
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_id, dek->dek_obj_id);
	/* [한국어] 조회 대상 obj_id. */

	rc = mlx5dv_devx_obj_query(dek->devx_obj, in, sizeof(in), out, sizeof(out));
	/* [한국어] DEVX 객체 조회 명령(객체 핸들 기반). */
	if (rc) {
		return rc;
	}

	dek_out = DEVX_ADDR_OF(query_encryption_key_obj_out, out, obj);
	/* [한국어] 응답 버퍼에서 객체 데이터 영역 시작 주소. */
	attr->state = DEVX_GET(encryption_key_obj, dek_out, state);
	/* [한국어] state 추출(READY=0x1, ERROR=...). */
	memcpy(&attr->opaque, DEVX_ADDR_OF(encryption_key_obj, dek_out, opaque), sizeof(attr->opaque));
	/* [한국어] opaque 8B 회수 — 등록 시 보낸 0과 일치해야 함. */

	return 0;
}

/*
 * [한국어]
 * spdk_mlx5_crypto_keytag_create - 평문 키로부터 모든 crypto-capable 디바이스에 DEK를 등록(공개 API).
 *
 * @attr: 입력. dek 포인터 + dek_len(32/40/64/72) + tweak/keytag 정보 등.
 * @out: 출력. 생성된 keytag 핸들.
 * @return: 0 성공, -EINVAL/-ENOMEM/-ENOTSUP 등.
 *
 * 동작:
 *   1. dek_len에 따라 key_size 와 keytag 사용 여부 결정.
 *   2. crypto-capable 디바이스 목록 조회.
 *   3. keytag 구조체 + deks 배열 할당.
 *   4. 각 디바이스에 대해: PD 가져오기 → cap 재조회 → DEK init → state 검증 → tweak_mode 결정.
 *   5. keytag 사용 시 raw 버퍼 끝 8B를 keytag.keytag에 보관.
 *
 * 한 디바이스라도 실패하면 keytag 전체를 destroy.
 */
int
spdk_mlx5_crypto_keytag_create(struct spdk_mlx5_crypto_dek_create_attr *attr,
			       struct spdk_mlx5_crypto_keytag **out)
{
	struct mlx5_crypto_dek *dek;
	struct spdk_mlx5_crypto_keytag *keytag;
	struct ibv_context **devs;
	struct ibv_pd *pd;
	struct mlx5_crypto_dek_init_attr dek_attr = {};
	struct mlx5_crypto_dek_query_attr query_attr;
	struct spdk_mlx5_device_caps dev_caps;
	int num_devs = 0, i, rc;

	dek_attr.dek = attr->dek;
	/* [한국어] 평문 raw 버퍼 포인터(호출자 제공). */
	dek_attr.key_size_bytes = attr->dek_len;
	/* [한국어] raw 버퍼 길이. */
	dek_attr.opaque = 0;
	/* [한국어] opaque 0으로 고정 — query 시 0이 그대로 돌아오는지 확인. */
	switch (dek_attr.key_size_bytes) {
	/* [한국어] 4가지 허용 길이만 인식. 그 외는 EINVAL. */
	case SPDK_MLX5_AES_XTS_128_DEK_BYTES_WITH_KEYTAG:
		dek_attr.key_size = MLX5_ENCRYPTION_KEY_OBJ_KEY_SIZE_SIZE_128;
		dek_attr.keytag = 1;
		SPDK_DEBUGLOG(mlx5, "128b AES_XTS with keytag\n");
		break;
	case SPDK_MLX5_AES_XTS_256_DEK_BYTES_WITH_KEYTAG:
		dek_attr.key_size = MLX5_ENCRYPTION_KEY_OBJ_KEY_SIZE_SIZE_256;
		dek_attr.keytag = 1;
		SPDK_DEBUGLOG(mlx5, "256b AES_XTS with keytag\n");
		break;
	case SPDK_MLX5_AES_XTS_128_DEK_BYTES:
		dek_attr.key_size = MLX5_ENCRYPTION_KEY_OBJ_KEY_SIZE_SIZE_128;
		dek_attr.keytag = 0;
		SPDK_DEBUGLOG(mlx5, "128b AES_XTS\n");
		break;
	case SPDK_MLX5_AES_XTS_256_DEK_BYTES:
		dek_attr.key_size = MLX5_ENCRYPTION_KEY_OBJ_KEY_SIZE_SIZE_256;
		dek_attr.keytag = 0;
		SPDK_DEBUGLOG(mlx5, "256b AES_XTS\n");
		break;
	default:
		SPDK_ERRLOG("Invalid key length %zu. The following keys are supported:\n"
			    "128b key + key2, %u bytes;\n"
			    "256b key + key2, %u bytes\n"
			    "128b key + key2 + keytag, %u bytes\n"
			    "256b lye + key2 + keytag, %u bytes\n",
			    attr->dek_len, SPDK_MLX5_AES_XTS_128_DEK_BYTES, SPDK_MLX5_AES_XTS_256_DEK_BYTES,
			    SPDK_MLX5_AES_XTS_128_DEK_BYTES_WITH_KEYTAG, SPDK_MLX5_AES_XTS_256_DEK_BYTES_WITH_KEYTAG);
		/* [한국어] 사용자에게 허용 사이즈를 친절히 안내. */
		return -EINVAL;
	}

	devs = spdk_mlx5_crypto_devs_get(&num_devs);
	/* [한국어] crypto-capable 디바이스 목록 조회. */
	if (!devs || !num_devs) {
		SPDK_DEBUGLOG(mlx5, "No crypto devices found\n");
		return -ENOTSUP;
	}

	keytag = calloc(1, sizeof(*keytag));
	if (!keytag) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_mlx5_crypto_devs_release(devs);
		return -ENOMEM;
	}
	keytag->deks = calloc(num_devs, sizeof(struct mlx5_crypto_dek));
	/* [한국어] 디바이스 수만큼 DEK 슬롯 사전 할당(0으로 초기화). */
	if (!keytag->deks) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_mlx5_crypto_devs_release(devs);
		free(keytag);
		return -ENOMEM;
	}

	for (i = 0; i < num_devs; i++) {
		keytag->deks_num++;
		/* [한국어] 슬롯 사용 카운트 증가. 실패 시 이만큼만 destroy하면 부분 정리 가능. */
		dek = &keytag->deks[i];
		pd = spdk_rdma_utils_get_pd(devs[i]);
		/* [한국어] 디바이스용 PD를 캐시에서 가져옴(없으면 새로 alloc). 같은 PD 재사용으로 자원 절약. */
		if (!pd) {
			SPDK_ERRLOG("Failed to get PD on device %s\n", devs[i]->device->name);
			rc = -EINVAL;
			goto err_out;
		}

		memset(&dev_caps, 0, sizeof(dev_caps));
		rc =  spdk_mlx5_device_query_caps(devs[i], &dev_caps);
		/* [한국어] cap 재조회 — tweak_mode 결정에 multi_block_be_tweak 비트 사용. */
		if (rc) {
			SPDK_ERRLOG("Failed to get device %s crypto caps\n", devs[i]->device->name);
			goto err_out;
		}
		rc = mlx5_crypto_dek_init(pd, &dek_attr, dek);
		/* [한국어] NIC에 평문 키 등록 — 성공 시 obj_id 받음. */
		if (rc) {
			SPDK_ERRLOG("Failed to create DEK on dev %s, rc %d\n", pd->context->device->name, rc);
			goto err_out;
		}
		memset(&query_attr, 0, sizeof(query_attr));
		rc = mlx5_crypto_dek_query(dek, &query_attr);
		/* [한국어] 등록 후 즉시 상태 조회 — READY 확인. */
		if (rc) {
			SPDK_ERRLOG("Failed to query DEK on dev %s, rc %d\n", pd->context->device->name, rc);
			goto err_out;
		}
		if (query_attr.opaque != 0 || query_attr.state != MLX5_ENCRYPTION_KEY_OBJ_STATE_READY) {
			/* [한국어] opaque 변조됐거나 state가 READY가 아니면 키 사용 불가. */
			SPDK_ERRLOG("DEK on dev %s in bad state %d, oapque %"PRIu64"\n", pd->context->device->name,
				    query_attr.state, query_attr.opaque);
			rc = -EINVAL;
			goto err_out;
		}

		dek->pd = pd;
		dek->context = devs[i];
		dek->tweak_mode = dev_caps.crypto.multi_block_be_tweak ?
				  SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_BE : SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_LE;
		/* [한국어] tweak_mode 결정: BE multi-block 지원하면 BE, 아니면 LE.
		 * AES-XTS의 tweak는 LBA에서 파생되며 인코딩 방식이 NIC 펌웨어에 따라 다름. 데이터-플레인이 BSF
		 * xts_initial_tweak에 16B 채울 때 이 모드에 따라 BE/LE 변환. */
	}

	if (dek_attr.keytag) {
		/* Save keytag, it will be used to configure crypto MKEY */
		/* [한국어] keytag 사용 모드면 raw 버퍼 끝 8B를 keytag.keytag로 복사 보관.
		 * 데이터-플레인이 inline crypto MKEY 등록 시 BSF.keytag로 채워 NIC에 전달. */
		keytag->has_keytag = true;
		memcpy(keytag->keytag, attr->dek + attr->dek_len - SPDK_MLX5_AES_XTS_KEYTAG_SIZE,
		       SPDK_MLX5_AES_XTS_KEYTAG_SIZE);
	}

	spdk_mlx5_crypto_devs_release(devs);
	*out = keytag;

	return 0;

err_out:
	spdk_mlx5_crypto_keytag_destroy(keytag);
	/* [한국어] 부분 등록된 DEK 모두 정리. */
	spdk_mlx5_crypto_devs_release(devs);

	return rc;
}

/*
 * [한국어]
 * mlx5_crypto_get_dek_by_pd - keytag 안의 deks[]에서 PD가 일치하는 DEK 검색(linear).
 *
 * @keytag: 검색 대상 keytag.
 * @pd: 매칭 PD.
 * @return: 발견된 DEK 또는 NULL.
 *
 * 디바이스 수가 보통 1~수개로 작아 linear search로 충분. 핫 패스 호출은 spdk_likely 힌트로
 * 단일 디바이스 가정 최적화 가능하지만 본 함수에는 적용 안 함.
 */
static inline struct mlx5_crypto_dek *
mlx5_crypto_get_dek_by_pd(struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd)
{
	struct mlx5_crypto_dek *dek;
	uint32_t i;

	for (i = 0; i < keytag->deks_num; i++) {
		dek = &keytag->deks[i];
		if (dek->pd == pd) {
			return dek;
			/* [한국어] PD 정확 일치 발견. */
		}
	}

	return NULL;
}

/*
 * [한국어]
 * spdk_mlx5_crypto_get_dek_data - PD로부터 데이터-플레인용 DEK 메타데이터 조회(공개 API).
 *
 * @keytag: keytag 핸들.
 * @pd: 사용할 PD.
 * @data: 출력. dek_obj_id + tweak_mode.
 * @return: 0 성공, -EINVAL(PD에 매칭되는 DEK 없음).
 *
 * 데이터-플레인(예: bdev_crypto_submit_request)이 inline crypto MKEY를 등록할 때마다 호출.
 * 핫 패스이므로 spdk_unlikely로 미스 분기 표시.
 */
int
spdk_mlx5_crypto_get_dek_data(struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd,
			      struct spdk_mlx5_crypto_dek_data *data)
{
	struct mlx5_crypto_dek *dek;

	dek = mlx5_crypto_get_dek_by_pd(keytag, pd);
	if (spdk_unlikely(!dek)) {
		SPDK_ERRLOG("No DEK for pd %p (dev %s)\n", pd, pd->context->device->name);
		return -EINVAL;
		/* [한국어] PD 미스 — 호출자가 잘못된 PD를 넘겼거나 keytag 생성 시 그 디바이스를 빠뜨렸음. */
	}
	data->dek_obj_id = dek->dek_obj_id;
	/* [한국어] BSF.dek_pointer에 들어갈 obj_id. */
	data->tweak_mode = dek->tweak_mode;
	/* [한국어] tweak 인코딩 방식 — BSF.xts_initial_tweak 채울 때 사용. */

	return 0;
}

/* [한국어] mlx5 로그 컴포넌트 등록. SPDK_DEBUGLOG(mlx5, ...) 사용 활성화에 필요.
 * spdk_log_set_flag("mlx5")로 켜면 본 파일의 디버그 로그가 출력됨. */
SPDK_LOG_REGISTER_COMPONENT(mlx5)
