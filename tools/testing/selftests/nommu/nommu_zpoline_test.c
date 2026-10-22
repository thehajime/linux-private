// SPDX-License-Identifier: GPL-2.0
/* UML SAS: deferred syscall rewriting must preserve RX and private mappings. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest.h"

#ifdef __x86_64__
static const unsigned char getpid_code[] = {
	0x48, 0xc7, 0xc0, 39, 0, 0, 0, 0x0f, 0x05, 0xc3
};

static unsigned char *new_code(size_t size, size_t offset, int invalid)
{
	unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	memcpy(p + offset, getpid_code, sizeof(getpid_code));
	if (invalid)
		memset(p + offset + 3, 0xff, 4);
	if (mprotect(p, size, PROT_READ | PROT_EXEC))
		ksft_exit_fail_msg("mprotect: %s\n", strerror(errno));
	return p;
}

static long invoke(unsigned char *p)
{
	return ((long (*)(void))p)();
}

static int raw(const unsigned char *p)
{
	return p[7] == 0x0f && p[8] == 0x05;
}

static int translated(const unsigned char *p)
{
	return p[7] == 0xff && p[8] == 0xd0;
}

int main(int argc, char **argv)
{
	size_t page = sysconf(_SC_PAGESIZE);
	unsigned char *p, *cross, *invalid, *private;
	int status, mode = argc > 1 ? atoi(argv[1]) : -1;
	pid_t child;
	long pid = getpid();

	ksft_print_header();
	ksft_set_plan(8);
	p = new_code(page, 0, 0);
	ksft_test_result(raw(p), "new RX code contains syscall\n");
	ksft_test_result(invoke(p) == pid, "RX syscall returns guest PID\n");
	if (mode < 0)
		mode = translated(p);
	ksft_test_result(mode ? translated(p) : raw(p),
			 "instruction matches zpoline mode\n");
	for (int i = 0; i < 10000; i++)
		if (invoke(p) != pid)
			ksft_exit_fail_msg("repeated getpid differs\n");
	ksft_test_result_pass("10000 repeated calls\n");

	private = new_code(page, 0, 0);
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (!child)
		_exit(invoke(private) == getpid() &&
		      (mode == 1 ? translated(private) : raw(private)) ? 0 : 1);
	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("waitpid: %s\n", strerror(errno));
	ksft_test_result(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
			 raw(private), "child rewrite leaves parent text unchanged\n");

	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (!child) {
		*(volatile unsigned char *)p = 0;
		_exit(1);
	}
	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("waitpid: %s\n", strerror(errno));
	ksft_test_result(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
			 "RX permissions still reject user writes\n");

	cross = new_code(2 * page, page - 8, 0);
	ksft_test_result(invoke(cross + page - 8) == pid &&
			 raw(cross + page - 8), "cross-page syscall retains fallback\n");
	invalid = new_code(page, 0, 1);
	ksft_test_result(invoke(invalid) == -ENOSYS && raw(invalid),
			 "invalid syscall stays on ENOSYS fallback\n");
	munmap(p, page);
	munmap(private, page);
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
