// SPDX-License-Identifier: GPL-2.0
/* Plain UML SAS: rewrite only exclusively owned, copied executable text. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../kselftest.h"

#ifdef __x86_64__
static const unsigned char code[] = {
	0x48, 0xc7, 0xc0, 39, 0, 0, 0, 0x0f, 0x05, 0xc3
};

extern long native_getpid(void);
extern const unsigned char native_getpid_site[];
asm(".pushsection .text\n"
    ".balign 16\n"
    ".global native_getpid\n"
    ".type native_getpid,@function\n"
    "native_getpid:\n"
    "mov $39, %rax\n"
    ".global native_getpid_site\n"
    "native_getpid_site:\n"
    "syscall\n"
    "ret\n"
    ".size native_getpid, .-native_getpid\n"
    ".popsection\n");

static int instruction(const unsigned char *p, int mode)
{
	return mode ? p[0] == 0xff && p[1] == 0xd0 :
		      p[0] == 0x0f && p[1] == 0x05;
}

static long invoke(unsigned char *p)
{
	return ((long (*)(void))p)();
}

static unsigned char *anonymous_code(size_t size, size_t offset, int invalid)
{
	unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		ksft_exit_fail_msg("anonymous mmap: %s\n", strerror(errno));
	memcpy(p + offset, code, sizeof(code));
	if (invalid)
		memset(p + offset + 3, 0xff, 4);
	return p;
}

static int file_code(const char *path, int shared, int mode, long pid, size_t page)
{
	unsigned char original[sizeof(code)], *p;
	int fd, good;

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (fd < 0 || ftruncate(fd, page) ||
	    pwrite(fd, code, sizeof(code), 0) != sizeof(code))
		ksft_exit_fail_msg("create %s: %s\n", path, strerror(errno));
	p = mmap(NULL, page, PROT_READ | PROT_EXEC,
		 shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED)
		ksft_exit_fail_msg("file mmap %s: %s\n", path, strerror(errno));
	good = invoke(p) == pid && instruction(p + 7, shared ? 0 : mode);
	good &= pread(fd, original, sizeof(original), 0) == sizeof(original) &&
		!memcmp(original, code, sizeof(code));
	munmap(p, page);
	close(fd);
	unlink(path);
	return good;
}

int main(int argc, char **argv)
{
	size_t page = sysconf(_SC_PAGESIZE);
	unsigned char *p, *cross, *invalid;
	long pid = getpid();
	int mode, good = 1;

	if (argc != 4)
		ksft_exit_fail_msg("usage: %s zpoline-mode hostfs-file ramfs-file\n",
				   argv[0]);
	mode = atoi(argv[1]);
	ksft_print_header();
	ksft_set_plan(9);
	ksft_test_result(instruction(native_getpid_site, 0), "ELF text starts raw\n");
	ksft_test_result(native_getpid() == pid, "ELF syscall returns guest PID\n");
	ksft_test_result(instruction(native_getpid_site, mode),
			 "ELF text matches zpoline mode\n");
	for (int i = 0; i < 10000; i++)
		good &= native_getpid() == pid;
	ksft_test_result(good, "10000 repeated ELF calls\n");
	p = anonymous_code(page, 0, 0);
	ksft_test_result(invoke(p) == pid && instruction(p + 7, mode),
			 "owned anonymous executable copy\n");
	cross = anonymous_code(2 * page, page - 8, 0);
	ksft_test_result(invoke(cross + page - 8) == pid &&
			 instruction(cross + page - 1, 0), "cross-page fallback\n");
	invalid = anonymous_code(page, 0, 1);
	ksft_test_result(invoke(invalid) == -ENOSYS && instruction(invalid + 7, 0),
			 "invalid syscall retains fallback\n");
	ksft_test_result(file_code(argv[2], 0, mode, pid, page),
			 "hostfs private copy leaves file unchanged\n");
	ksft_test_result(file_code(argv[3], 1, mode, pid, page),
			 "shared ramfs text and file remain unchanged\n");
	for (int rep = 0; rep < 3; rep++) {
		struct timespec start, end;
		long long elapsed;

		if (clock_gettime(CLOCK_MONOTONIC, &start))
			ksft_exit_fail_msg("clock_gettime: %s\n", strerror(errno));
		for (int i = 0; i < 1000000; i++)
			if (native_getpid() != pid)
				ksft_exit_fail_msg("benchmark PID differs\n");
		if (clock_gettime(CLOCK_MONOTONIC, &end))
			ksft_exit_fail_msg("clock_gettime: %s\n", strerror(errno));
		elapsed = (end.tv_sec - start.tv_sec) * 1000000000LL +
			  end.tv_nsec - start.tv_nsec;
		ksft_print_msg("getpid: rep=%d pid=%ld site=%p ns/call=%.3f\n",
			       rep + 1, pid, native_getpid_site, elapsed / 1000000.0);
	}
	munmap(p, page);
	munmap(cross, 2 * page);
	munmap(invalid, page);
	ksft_finished();
}
#else
int main(void)
{
	ksft_exit_skip("x86-64 UML SAS only\n");
}
#endif
