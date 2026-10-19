// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/nommu_swmmu.h>
#include <asm/unistd.h>
#include "kselftest.h"

#ifndef __x86_64__
int main(void)
{
	ksft_exit_skip("direct instruction test requires x86_64\n");
}
#else
static uint64_t direct_load(const void *p)
{
	uint64_t v;

	__asm__ volatile("movq (%1), %0" : "=r"(v) : "r"(p) : "memory");
	return v;
}

static void direct_store(void *p, uint64_t v)
{
	__asm__ volatile("movq %1, (%0)" : : "r"(p), "r"(v) : "memory");
}

static int expected_fault_code;

static void fault_handler(int sig, siginfo_t *info, void *ctx)
{
	_exit(sig == SIGSEGV && info->si_code == expected_fault_code ? 0 : 5);
}

static int expect_fault(void *p, int write)
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return 0;
	if (!pid) {
		struct sigaction action = { .sa_sigaction = fault_handler,
			.sa_flags = SA_SIGINFO };

		expected_fault_code = write ? SEGV_ACCERR : SEGV_MAPERR;
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGSEGV, &action, NULL))
			_exit(6);
		if (write)
			direct_store(p, 99);
		else
			(void)direct_load(p);
		_exit(1);
	}
	return waitpid(pid, &status, 0) == pid &&
		WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int fork_isolation(uint64_t *p)
{
	int to_parent[2], to_child[2], status;
	char token;
	pid_t pid;

	if (pipe(to_parent) || pipe(to_child))
		return 0;
	direct_store(p, 41);
	pid = fork();
	if (pid < 0)
		return 0;
	if (!pid) {
		close(to_parent[0]);
		close(to_child[1]);
		if (direct_load(p) != 41)
			_exit(2);
		direct_store(p, 42);
		if (write(to_parent[1], "x", 1) != 1 ||
		    read(to_child[0], &token, 1) != 1)
			_exit(3);
		_exit(direct_load(p) == 42 ? 0 : 4);
	}
	close(to_parent[1]);
	close(to_child[0]);
	int ok = read(to_parent[0], &token, 1) == 1 && direct_load(p) == 41;

	direct_store(p, 43);
	ok &= write(to_child[1], "x", 1) == 1;
	close(to_parent[0]);
	close(to_child[1]);
	ok &= waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
		WEXITSTATUS(status) == 0;
	return ok;
}

int main(void)
{
	uint64_t bounds[2] = { 0 }, observed = 0;
	size_t page = sysconf(_SC_PAGESIZE);
	uint64_t *p, *q;
	int ok;

	ksft_print_header();
	if (syscall(__NR_nommu_swmmu_load, NULL, sizeof(bounds), bounds,
		    NOMMU_SWMMU_QUERY_DIRECT_WINDOW))
		ksft_exit_skip("kernel direct window unavailable: %m\n");
	ksft_set_plan(7);
	ksft_test_result(bounds[0] && bounds[0] < bounds[1],
		"kernel-provided window %llx-%llx\n",
		(unsigned long long)bounds[0], (unsigned long long)bounds[1]);
	p = mmap(NULL, page, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED || (uintptr_t)p < bounds[0] ||
	    (uintptr_t)p + page > bounds[1])
		ksft_exit_fail_msg("mmap did not provide a window alias\n");
	direct_store(p, 23);
	ok = !syscall(__NR_nommu_swmmu_load, p, sizeof(observed), &observed,
		      NOMMU_SWMMU_ACCESS_DYNAMIC) && observed == 23;
	ksft_test_result(ok, "direct and helper backing agree\n");
	ok = 1;
	for (int i = 0; i < 3; i++)
		ok &= fork_isolation(p);
	ksft_test_result(ok, "parent/child direct stores remain isolated across three forks\n");
	ok = !mprotect(p, page, PROT_READ) && expect_fault(p, 1);
	ksft_test_result(ok, "read-only alias rejects direct write\n");
	ok = !munmap(p, page) && expect_fault(p, 0);
	ksft_test_result(ok, "unmapped alias retains an inaccessible guard\n");
	q = mmap(p, page, PROT_READ | PROT_WRITE,
		 MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ok = q == p && direct_load(q) == 0;
	ksft_test_result(ok, "fixed replacement has fresh direct backing\n");
	if (q == MAP_FAILED)
		ksft_exit_fail_msg("replacement mmap failed\n");
	direct_store(q, 75);
	p = mremap(q, page, 2 * page, MREMAP_MAYMOVE);
	ok = p != MAP_FAILED && direct_load(p) == 75;
	if (ok) {
		direct_store((char *)p + page, 76);
		ok = direct_load((char *)p + page) == 76;
		munmap(p, 2 * page);
	}
	ksft_test_result(ok, "remap preserves data and publishes expanded aliases\n");
	ksft_finished();
}
#endif
