// SPDX-License-Identifier: GPL-2.0
/* UML SAS: deferred syscall rewriting must preserve RX and private mappings. */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <ucontext.h>

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

static void fp_tick(int sig)
{
	(void)sig;
}

static int fp_probe(uint64_t pattern, unsigned int width)
{
	struct sigaction action = { .sa_handler = fp_tick, .sa_flags = SA_RESTART };
	struct sigaction old_action;
	struct itimerval timer = { { 0, 1000 }, { 0, 1000 } }, old_timer;
	uint64_t wanted[8], actual[8];

	unsigned int errors = 0;
	long ret;

	for (unsigned int i = 0; i < 8; i++)
		wanted[i] = pattern ^ (0x123456789abcdefULL * i);

	if (sigaction(SIGALRM, &action, &old_action) ||
	    setitimer(ITIMER_REAL, &timer, &old_timer))
		return 1;
	for (unsigned int i = 0; i < 10000; i++) {
#define FP_ROUNDTRIP(insn, reg) do { \
		asm volatile(insn " %2, %%" reg "15\n\t" \
			     "syscall\n\t" \
			     insn " %%" reg "15, %0" \
			     : "=m"(actual), "=a"(ret) \
			     : "m"(wanted), "1"((long)SYS_sched_yield) \
			     : "xmm15", "rcx", "r11", "memory"); \
	} while (0)
		if (width == 64)
			FP_ROUNDTRIP("vmovdqu64", "zmm");
		else if (width == 32)
			FP_ROUNDTRIP("vmovdqu", "ymm");
		else
			FP_ROUNDTRIP("movdqu", "xmm");
#undef FP_ROUNDTRIP
		if (ret || memcmp(actual, wanted, width))
			errors++;
	}
	if (setitimer(ITIMER_REAL, &old_timer, NULL) ||
	    sigaction(SIGALRM, &old_action, NULL))
		return 1;
	if (errors)
		ksft_print_msg("FP context mismatches: %u/10000\n", errors);
	return errors != 0;
}

static int fp_process_context(unsigned int width)
{
	pid_t child;
	int status, failed;

	fflush(stdout);
	child = fork();
	if (child < 0)
		return 0;
	if (!child) {
		failed = fp_probe(0x0123456789abcdefULL, width);
		fflush(stdout);
		_exit(failed);
	}
	failed = fp_probe(0xfedcba9876543210ULL, width);
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			return 0;
	}
	return !failed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int vector_supported(unsigned int width)
{
	unsigned int a = 1, b, c, d;
	uint64_t enabled;

	asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
	if ((c & ((1U << 27) | (1U << 28))) != ((1U << 27) | (1U << 28)))
		return 0;
	asm volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
	enabled = (uint64_t)d << 32 | a;
	if ((enabled & 6) != 6)
		return 0;
	if (width == 32)
		return 1;
	a = 7;
	c = 0;
	asm volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
	return (b & (1U << 16)) && (enabled & 0xe6) == 0xe6;
}

static volatile sig_atomic_t malformed_kind;

static void malformed_fp_handler(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	unsigned char *fp = (void *)uc->uc_mcontext.fpregs;

	(void)sig;
	(void)info;
	if (malformed_kind == 0)
		*(uint32_t *)(fp + 24) |= 1U << 31;
	else if (malformed_kind == 1)
		*(uint64_t *)(fp + 512) |= 1ULL << 63;
	else if (malformed_kind == 2)
		*(uint64_t *)(fp + 520) = 1ULL << 63;
	else
		*(uint64_t *)(fp + 528) = 1;
}

static int malformed_fp_context(int kind)
{
	struct sigaction action = {
		.sa_sigaction = malformed_fp_handler,
		.sa_flags = SA_SIGINFO,
	};
	pid_t child;
	int status;

	fflush(stdout);
	child = fork();
	if (child < 0)
		return 0;
	if (!child) {
		malformed_kind = kind;
		if (sigaction(SIGUSR1, &action, NULL) || raise(SIGUSR1))
			_exit(1);
		_exit(2);
	}
	while (waitpid(child, &status, 0) < 0)
		if (errno != EINTR)
			return 0;
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

int main(int argc, char **argv)
{
	size_t page = sysconf(_SC_PAGESIZE);
	unsigned char *p, *cross, *invalid, *private;
	int status, mode = argc > 1 ? atoi(argv[1]) : -1;
	pid_t child;
	long pid = getpid();

	ksft_print_header();
	ksft_set_plan(15);
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
	ksft_test_result(fp_process_context(16),
			 "FP state survives scheduling and signal delivery\n");
	for (unsigned int width = 32; width <= 64; width *= 2) {
		if (!vector_supported(width))
			ksft_test_result_skip("%u-byte vector state unsupported\n", width);
		else
			ksft_test_result(fp_process_context(width),
					 "%u-byte vector state survives scheduling and signals\n",
					 width);
	}

	for (int kind = 0; kind < 4; kind++) {
		if (kind && !vector_supported(32))
			ksft_test_result_skip("XSTATE frame unsupported (%d)\n", kind);
		else
			ksft_test_result(malformed_fp_context(kind),
					 "malformed FP signal frame rejected (%d)\n", kind);
	}

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
