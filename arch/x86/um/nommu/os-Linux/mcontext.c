// SPDX-License-Identifier: GPL-2.0
#include <sys/ucontext.h>
#define __FRAME_OFFSETS
#include <linux/errno.h>
#include <asm/ptrace.h>
#include <sysdep/ptrace.h>
#include <sysdep/mcontext.h>
#include <os.h>
#include "../syscalls.h"

extern long __kernel_vsyscall(int64_t a0, int64_t a1, int64_t a2, int64_t a3,
			      int64_t a4, int64_t a5, int64_t a6);

void set_mc_relay_signal(mcontext_t *mc)
{
	/* configure stack and userspace returning routine as
	 * instruction pointer
	 */
	mc->gregs[REG_RSP] = (unsigned long) current_top_of_stack;
	mc->gregs[REG_RIP] = (unsigned long) userspace;
}

void set_mc_sigsys_hook(mcontext_t *mc)
{
	os_x86_set_hostfs();
	mc->gregs[REG_RCX] = mc->gregs[REG_RIP];
	mc->gregs[REG_RIP] = (unsigned long) __kernel_vsyscall;
}

void zpoline_set_invalid_syscall_return(void *_mc, unsigned long return_ip,
					unsigned long stack_pointer)
{
	mcontext_t *mc = _mc;

	mc->gregs[REG_RAX] = -ENOSYS;
	mc->gregs[REG_RCX] = return_ip;
	mc->gregs[REG_R11] = mc->gregs[REG_EFL];
	mc->gregs[REG_RIP] = return_ip;
	mc->gregs[REG_RSP] = stack_pointer + sizeof(unsigned long);
}
