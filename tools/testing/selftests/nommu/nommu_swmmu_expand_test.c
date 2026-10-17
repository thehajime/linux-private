// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nommu_swmmu_user.h"

/* Keep the all-access plugin's runtime declarations visible in this TU. */
static uint64_t (* const __attribute__((used))
keep_swmmu_load_return_value)(const void *, size_t) =
	nommu_swmmu_load_return_value;
static long (* const __attribute__((used))
keep_swmmu_store_dynamic)(void *, size_t, uint64_t) =
	nommu_swmmu_store_dynamic;

/*
 * Focused regression reproducer for SWMMU alias invalidation on in-place
 * mremap growth. This keeps the hot expansion/store sequence small enough to
 * run after each kernel change; the full NOMMU SWMMU suite remains necessary.
 */
int main(void)
{
	const uint64_t old_value = 0x1122334455667788ULL;
	const uint64_t tail_value = 0xaabbccddeeff0011ULL;
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	void *mapping, *expanded;
	uint64_t value;
	long ret;

	if (!page_size) {
		perror("sysconf(_SC_PAGESIZE)");
		return 1;
	}

	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	ret = nommu_swmmu_store_u64(mapping, sizeof(old_value), old_value);
	if (ret < 0) {
		fprintf(stderr, "initial SWMMU store failed: %s\n", strerror(errno));
		goto fail_unmap;
	}

	expanded = mremap(mapping, page_size, page_size * 2, 0);
	if (expanded == MAP_FAILED) {
		perror("mremap growth");
		goto fail_unmap;
	}
	if (expanded != mapping) {
		fprintf(stderr, "mremap moved mapping: %p -> %p\n", mapping,
			expanded);
		munmap(expanded, page_size * 2);
		return 1;
	}

	value = nommu_swmmu_load_u64(mapping, sizeof(value));
	if (value != old_value) {
		fprintf(stderr, "old page changed: %#llx\n",
			(unsigned long long)value);
		goto fail_expanded;
	}

	ret = nommu_swmmu_store_u64((char *)mapping + page_size,
				    sizeof(tail_value), tail_value);
	if (ret < 0) {
		fprintf(stderr, "expanded-tail SWMMU store failed: %s\n",
			strerror(errno));
		goto fail_expanded;
	}
	value = nommu_swmmu_load_u64((char *)mapping + page_size,
				     sizeof(value));
	if (value != tail_value) {
		fprintf(stderr, "expanded tail mismatch: %#llx\n",
			(unsigned long long)value);
		goto fail_expanded;
	}

	if (munmap(mapping, page_size * 2)) {
		perror("munmap");
		return 1;
	}
	puts("SWMMU mremap expansion alias regression: PASS");
	return 0;

fail_expanded:
	munmap(mapping, page_size * 2);
	return 1;
fail_unmap:
	munmap(mapping, page_size);
	return 1;
}
