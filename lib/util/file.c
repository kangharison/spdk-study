/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/*
 * [한국어 설명] 파일 전체 로드 + sysfs 속성 읽기 헬퍼 (file.c)
 *
 * === 파일의 역할 ===
 * 두 종류의 파일 I/O 헬퍼를 묶은 유틸:
 *   1) 임의 길이 파일을 메모리로 한 번에 로드(`spdk_posix_file_load`,
 *      `spdk_posix_file_load_from_name`) — 시작 128KB로 출발해 1GB 상한까지
 *      두 배씩 키워가며 EOF까지 읽는다.
 *   2) Linux sysfs 속성(`/sys/.../{attr}`) 한 줄 읽기(`spdk_read_sysfs_attribute`,
 *      `spdk_read_sysfs_attribute_uint32`) — printf 스타일 경로 포맷팅 + 자동
 *      newline 트리밍 + 정수 파싱.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 콜드 패스(설정/시작 단계) 위주 헬퍼:
 *   - JSON 설정 파일 일괄 로드: spdk_posix_file_load_from_name → JSON 파서.
 *   - PCIe·NVMe 토폴로지 조회: lib/env_dpdk나 module/bdev/nvme가 sysfs로
 *     /sys/bus/pci/devices/.../driver, numa_node, queue_depth 등 속성 읽기.
 * 호출 흐름 예: SPDK 앱 시작 → spdk_app_parse_config → spdk_posix_file_load
 *   → JSON 텍스트 → spdk_json_parse → 서브시스템 초기화.
 *
 * === 타 모듈과의 연결 ===
 * - 의존: `spdk/file.h`(prototype), `spdk/string.h`(spdk_vsprintf_alloc,
 *   spdk_strtoll). 표준 stdio(fopen/fread/getline/fclose), stdlib(realloc/free).
 * - 호출자: app/* (설정 로드), module/bdev/nvme(sysfs PCIe 속성),
 *   lib/env_dpdk(NUMA·hugepage 정보).
 * - 공유 상태: 없음. 결과 버퍼는 호출자에게 소유권 이전.
 *
 * === 주요 함수/구조체 요약 ===
 * - spdk_posix_file_load(file, *size): 열려있는 FILE* 전체를 읽어 malloc된
 *   버퍼와 size를 반환. 1GB 초과 시 NULL.
 * - spdk_posix_file_load_from_name(name, *size): fopen 후 위 함수에 위임.
 * - read_sysfs_attribute(**attr_p, fmt, args) [static]: vprintf 포맷의 경로를
 *   조립해 한 줄(getline) 읽고, trailing '\n' 제거.
 * - spdk_read_sysfs_attribute(**attr_p, path_fmt, ...): variadic 래퍼.
 * - spdk_read_sysfs_attribute_uint32(*out, path_fmt, ...): 위 결과를
 *   spdk_strtoll로 파싱해 0..UINT32_MAX 검증.
 */

#include "spdk/file.h"
/* [한국어] 본 파일이 외부 노출하는 prototype. */
#include "spdk/string.h"
/* [한국어] spdk_vsprintf_alloc(printf 결과를 malloc 버퍼로 받는 헬퍼),
 * spdk_strtoll(strtol 안전판). */

/*
 * [한국어]
 * spdk_posix_file_load - 열려있는 FILE*의 전체 내용을 메모리로 로드.
 *
 * @file: 이미 fopen된 스트림.
 * @size: [out] 실제 읽은 byte 수.
 * @return: malloc된 버퍼 또는 NULL(메모리 부족/IO 에러/1GB 초과).
 *          호출자가 free 책임.
 *
 * 동기: SPDK 설정 JSON 등은 보통 작지만 가변이라 길이를 미리 알 수 없다.
 * 128KB → 256KB → ... 의 두 배씩 확장 전략으로 realloc 횟수와 마지막
 * 메모리 낭비를 모두 제어. 1GB 상한은 비정상 입력으로부터 OOM 방지.
 *
 * 실행 컨텍스트: 콜드 패스(앱 시작). 호출 후 호출자가 NUL 종결 등 추가
 * 처리를 할 수 있다(반환 버퍼는 raw 바이트만 담는다).
 *
 * 호출 체인: 호출자 → spdk_posix_file_load → fread/realloc → ferror/feof.
 */
void *
spdk_posix_file_load(FILE *file, size_t *size)
{
	uint8_t *newbuf, *buf = NULL;
	/* [한국어] buf: 누적 결과 포인터(NULL부터 시작 → realloc이 malloc처럼 동작).
	 * newbuf: realloc 결과를 임시로 받아 실패 시 buf 보존(누수 방지). */
	size_t rc, buf_size, cur_size = 0;
	/* [한국어] rc: 마지막 fread가 실제 읽은 바이트 수.
	 * buf_size: 현재 할당된 버퍼 용량.
	 * cur_size: 지금까지 누적된 실제 데이터 크기. */

	*size = 0;
	/* [한국어] 출력 변수 초기화 — 실패 시에도 정의된 값. */
	buf_size = 128 * 1024;
	/* [한국어] 시작 용량 128KB. JSON 설정 파일이 들어가기 충분한 크기. */

	while (buf_size <= 1024 * 1024 * 1024) {
		/* [한국어] 1GB까지만 확장. 비정상 거대 파일 방어. */
		newbuf = realloc(buf, buf_size);
		/* [한국어] 기존 데이터를 보존하며 buf_size로 확장.
		 * 실패 시 NULL — 이때 buf는 그대로이므로 따로 free해야 누수 방지. */
		if (newbuf == NULL) {
			free(buf);
			return NULL;
		}
		buf = newbuf;
		/* [한국어] 성공한 새 포인터를 buf로 채택. */

		rc = fread(buf + cur_size, 1, buf_size - cur_size, file);
		/* [한국어] 누적 위치 뒤에서부터 (남은 용량) 만큼 읽기 시도. */
		cur_size += rc;
		/* [한국어] 누적 크기 갱신. */

		if (feof(file)) {
			/* [한국어] EOF 도달 → 정상 종료. *size 갱신 후 버퍼 반환. */
			*size = cur_size;
			return buf;
		}

		if (ferror(file)) {
			/* [한국어] fread 중 IO 에러 → 누수 방지하며 NULL. */
			free(buf);
			return NULL;
		}

		buf_size *= 2;
		/* [한국어] EOF도 에러도 아니면 더 큰 용량 필요 → 버퍼 두 배 확장. */
	}

	free(buf);
	/* [한국어] 1GB 한계를 넘은 경우 누수 방지하고 NULL 반환. */
	return NULL;
}

/*
 * [한국어]
 * spdk_posix_file_load_from_name - 파일명을 받아 fopen + load + fclose 일괄.
 *
 * @file_name: 파일 경로.
 * @size: [out] 읽은 byte 수.
 * @return: malloc 버퍼 또는 NULL.
 *
 * 호출자 측 boilerplate(open/read/close)를 단축하기 위한 래퍼.
 *
 * 호출 체인: 호출자 → spdk_posix_file_load_from_name → fopen →
 *           spdk_posix_file_load → fclose.
 */
void *
spdk_posix_file_load_from_name(const char *file_name, size_t *size)
{
	FILE *file = fopen(file_name, "r");
	/* [한국어] 텍스트/바이너리 무관 read-only로 열기. 실패 시 NULL. */
	void *data;

	if (file == NULL) {
		/* [한국어] 파일 부재/권한 부족 등 → NULL 전파. errno는 호출자가
		 * 별도로 검사할 수 있다. */
		return NULL;
	}

	data = spdk_posix_file_load(file, size);
	/* [한국어] 실제 읽기는 위 함수에 위임. */
	fclose(file);
	/* [한국어] 결과 성패와 무관하게 fd 누수 방지를 위해 항상 close. */

	return data;
	/* [한국어] 로드 결과(혹은 실패 시 NULL) 반환. */
}

/*
 * [한국어]
 * read_sysfs_attribute [static] - /sys 속성 한 줄 읽기 헬퍼.
 *
 * @attribute_p: [out] 동적 할당된 문자열 포인터. 호출자가 free 책임.
 * @format/@args: vprintf 스타일 경로 포맷.
 * @return: 0 성공, -errno 실패.
 *
 * 동기: SPDK는 /sys/bus/pci/devices/0000:01:00.0/numa_node, queue_depth 같은
 * sysfs 한 줄짜리 속성을 자주 읽는다. 매번 sprintf+fopen+getline+'\n' 제거를
 * 반복하기 번거로워 한 함수로 묶음.
 *
 * 실행 컨텍스트: 콜드 패스(디바이스 enumeration 시).
 *
 * 호출 체인:
 *   bdev/nvme 또는 env_dpdk → spdk_read_sysfs_attribute(_uint32) →
 *   read_sysfs_attribute → fopen/getline.
 */
static int
read_sysfs_attribute(char **attribute_p, const char *format, va_list args)
{
	char *attribute;
	/* [한국어] 결과 문자열의 로컬 별칭(가독성). */
	FILE *file;
	/* [한국어] sysfs 파일 스트림. */
	char *path;
	/* [한국어] 포맷팅된 sysfs 경로. spdk_vsprintf_alloc이 malloc해 채움. */
	size_t len = 0;
	/* [한국어] getline에 넘길 버퍼 용량 추적 변수.
	 * NOTE: 아래에서 strlen 결과 저장에 재사용된다(주석 그대로). */
	ssize_t read;
	/* [한국어] getline 반환값(읽은 길이 또는 -1). */
	int errsv;
	/* [한국어] errno 저장(close 사이에 errno가 덮어써지는 것을 방지). */

	path = spdk_vsprintf_alloc(format, args);
	/* [한국어] printf 포맷의 경로(예: "/sys/bus/pci/devices/%s/numa_node")를
	 * malloc된 문자열로 받음. 실패 시 NULL. */
	if (path == NULL) {
		return -ENOMEM;
	}

	file = fopen(path, "r");
	/* [한국어] sysfs 속성 파일을 read-only 텍스트 모드로 open. */
	errsv = errno;
	/* [한국어] free(path)가 errno를 건드리면 안 되므로 즉시 백업. */
	free(path);
	/* [한국어] 경로 문자열은 더 이상 필요 없으므로 즉시 해제. */
	if (file == NULL) {
		assert(errsv != 0);
		/* [한국어] fopen 실패면 errno != 0이어야 정상. 디버그 빌드에서
		 * 위반 시 즉시 추적 가능. */
		return -errsv;
	}

	*attribute_p = NULL;
	/* [한국어] getline에 NULL 시드를 넘기면 자체 malloc하도록 요청. */
	read = getline(attribute_p, &len, file);
	/* [한국어] sysfs는 보통 한 줄짜리. getline이 newline까지 동적 할당. */
	errsv = errno;
	/* [한국어] fclose가 errno 변경 가능 → 백업. */
	fclose(file);
	/* [한국어] 결과와 무관하게 fd 정리. */
	attribute = *attribute_p;
	/* [한국어] 로컬 별칭으로 캡처. */
	if (read == -1) {
		/* getline man page says line should be freed even on failure. */
		/* [한국어] getline은 실패 시에도 첫 호출에서 buf를 할당했을 수 있어
		 * 매뉴얼에서 free를 권고. 누수 방지. */
		free(attribute);
		assert(errsv != 0);
		/* [한국어] -1이면 errno도 세팅되어야 정상. */
		return -errsv;
	}

	/* len is the length of the allocated buffer, which may be more than
	 * the string's length. Reuse len to hold the actual strlen.
	 */
	/* [한국어] getline의 len은 capacity인데 필요 없게 됐으므로 strlen 결과로
	 * 재사용해 변수 할당 절약. */
	len = strlen(attribute);
	if (attribute[len - 1] == '\n') {
		/* [한국어] sysfs는 대부분 trailing newline이 붙음. SPDK는 보통
		 * 비교/파싱이 목적이라 newline을 제거. */
		attribute[len - 1] = '\0';
	}

	return 0;
	/* [한국어] 성공. 호출자가 *attribute_p free 책임. */
}

/*
 * [한국어]
 * spdk_read_sysfs_attribute - variadic 래퍼: 포맷 인자를 받아
 *                              read_sysfs_attribute에 위임.
 *
 * @attribute_p: [out] malloc 문자열.
 * @path_format: printf 스타일 경로 포맷.
 * @...: 포맷 인자.
 * @return: 0 성공, -errno 실패.
 */
int
spdk_read_sysfs_attribute(char **attribute_p, const char *path_format, ...)
{
	va_list args;
	int rc;

	va_start(args, path_format);
	/* [한국어] variadic 인자 시작. */
	rc = read_sysfs_attribute(attribute_p, path_format, args);
	/* [한국어] 실제 작업은 static helper에 위임. */
	va_end(args);
	/* [한국어] variadic 정리. */

	return rc;
}

/*
 * [한국어]
 * spdk_read_sysfs_attribute_uint32 - sysfs 속성을 읽어 부호 없는 32비트 정수
 *                                    로 파싱.
 *
 * @attribute: [out] 변환된 정수 값.
 * @path_format: 경로 포맷.
 * @...: 포맷 인자.
 * @return: 0 성공, -errno(IO 실패), -EINVAL(범위 초과/음수).
 *
 * sysfs 한 줄 정수(예: numa_node, queue_depth)를 안전하게 읽기 위한 헬퍼.
 * 음수와 UINT32_MAX 초과를 모두 거부.
 */
int
spdk_read_sysfs_attribute_uint32(uint32_t *attribute, const char *path_format, ...)
{
	char *attribute_str = NULL;
	/* [한국어] 임시 문자열 버퍼. read_sysfs_attribute가 malloc. */
	long long int val;
	/* [한국어] strtoll 결과 (음수 검출과 UINT32_MAX 초과 검출을 위해 충분히 큰 타입). */
	va_list args;
	int rc;

	va_start(args, path_format);
	rc = read_sysfs_attribute(&attribute_str, path_format, args);
	/* [한국어] 우선 문자열로 읽기. */
	va_end(args);

	if (rc != 0) {
		/* [한국어] 읽기 실패 → 그대로 전파(문자열은 helper가 정리). */
		return rc;
	}

	val = spdk_strtoll(attribute_str, 0);
	/* [한국어] base 0 → "0x..." 16진/"0..." 8진/그 외 10진 자동 인식. */
	free(attribute_str);
	/* [한국어] 파싱 후 원본 문자열은 즉시 해제. */
	if (val < 0 || val > UINT32_MAX) {
		/* [한국어] uint32 범위를 벗어나면 호출자에게 EINVAL로 보고.
		 * sysfs 속성이 음수일 수도 있는데(numa_node = -1) 본 함수는
		 * uint32 전용이라 음수 거부. 호출자는 별도 long long 버전을
		 * 사용해야 한다. */
		return -EINVAL;
	}

	*attribute = (uint32_t)val;
	/* [한국어] 안전하게 캐스팅 후 출력. */
	return 0;
}
