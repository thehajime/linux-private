/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_NOMMU_SYSCALLS_H
#define __UM_NOMMU_SYSCALLS_H


#define task_top_of_stack(task) \
({									\
	unsigned long __ptr = (unsigned long)task->stack;	\
	__ptr += THREAD_SIZE;			\
	__ptr;					\
})

extern long current_top_of_stack;
extern long current_ptregs;

int os_x86_arch_prctl(int pid, int option, unsigned long *arg2);
void arch_save_guest_fp(void);
int arch_validate_guest_fp(const void *fp);
void arch_set_stack_to_current(void);
void arch_restore_guest_fs(void);
void os_x86_set_hostfs(void);
struct pt_regs;
void zpoline_finish_sigsys(struct pt_regs *regs);

#endif
