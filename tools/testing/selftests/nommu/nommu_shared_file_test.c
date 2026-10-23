// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../kselftest.h"

/* Run with TMPDIR on a filesystem supporting shared file mmap, e.g. tmpfs. */
int main(void)
{
	const char *tmpdir = getenv("TMPDIR");
	char path[4096];
	size_t size = sysconf(_SC_PAGESIZE);
	void *anon, *first, *second;
	int fd;

	ksft_print_header();
	ksft_set_plan(3);
	anon = mmap(NULL, size, PROT_READ | PROT_WRITE,
	            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (anon == MAP_FAILED)
		ksft_exit_fail_msg("anonymous shared mmap: %s\n", strerror(errno));
	memset(anon, 0x5a, size);
	if (snprintf(path, sizeof(path), "%s/nommu-shared-file-XXXXXX",
	             tmpdir ? tmpdir : "/tmp") >= (int)sizeof(path))
		ksft_exit_fail_msg("temporary path is too long\n");
	fd = mkstemp(path);
	if (fd < 0)
		ksft_exit_fail_msg("mkstemp: %s\n", strerror(errno));
	unlink(path);
	if (write(fd, anon, size) != (ssize_t)size)
		ksft_exit_fail_msg("write: %s\n", strerror(errno));
	first = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (first == MAP_FAILED) {
		if (errno == ENODEV || errno == ENOSYS || errno == EOPNOTSUPP)
			ksft_exit_skip("TMPDIR filesystem does not support shared file mmap\n");
		ksft_exit_fail_msg("first file mmap: %s\n", strerror(errno));
	}
	ksft_test_result(!memcmp(first, anon, size),
	                 "file mapping after anonymous shared region retains data\n");
	second = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (second == MAP_FAILED)
		ksft_exit_fail_msg("second file mmap: %s\n", strerror(errno));
	ksft_test_result(!memcmp(first, second, size),
	                 "repeated file mapping retains data\n");
	memset(anon, 0xa5, size);
	ksft_test_result(*(unsigned char *)first == 0x5a &&
	                 *(unsigned char *)second == 0x5a,
	                 "anonymous writes do not change either file mapping\n");
	if (second != first)
		munmap(second, size);
	munmap(first, size);
	munmap(anon, size);
	close(fd);
	ksft_finished();
}
