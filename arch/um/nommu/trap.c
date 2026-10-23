// SPDX-License-Identifier: GPL-2.0
/* SAS-only host fault classification and zpoline instruction recovery.
 * Common guest signal delivery stays in arch/um/kernel/trap.c.
 */
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <asm/unistd.h>
#include <asm/sas-trap.h>
#include <as-layout.h>
#include <kern_util.h>
#include <os.h>

bool sas_user_ip(unsigned long ip)
{
	if (ip > uml_reserved && ip < high_physmem)
		return true;
#ifdef CONFIG_NOMMU_SWMMU
	if (um_nommu_swmmu_address(ip) && ip < um_nommu_swmmu_user_end)
		return true;
#endif
	return false;
}

#ifdef CONFIG_64BIT
/*
 * zpoline rewrites syscall to call *%rax through the low-address trampoline.
 * An invalid number such as -1 faults at that target before reaching the
 * syscall dispatcher. Recognize only that rewritten call and emulate the
 * kernel's -ENOSYS return; unrelated faults keep the normal signal path.
 */
bool sas_zpoline_invalid_syscall(struct uml_pt_regs *regs,
						 struct siginfo *si, void *mc)
{
	struct faultinfo *fi = UPT_FAULTINFO(regs);
	unsigned long nr = UPT_AX(regs);
	unsigned long sp = UPT_SP(regs);
	unsigned long return_ip;
	u16 instruction;

	if (!um_zpoline_enabled || !si || !mc)
		return false;
	if ((long)nr >= 0 && nr < NR_syscalls)
		return false;
	if (UPT_IP(regs) != nr || si->si_addr != (void *)nr)
		return false;
	/* The target fault must be a user-mode instruction fetch. */
	if (fi->trap_no != 14 || (fi->error_code & 0x14) != 0x14)
		return false;
	if (sp > ULONG_MAX - sizeof(return_ip) ||
	    !sas_user_ip(sp) || !sas_user_ip(sp + sizeof(return_ip) - 1))
		return false;
	if (copy_from_user_nofault(&return_ip, (void __user *)sp,
				   sizeof(return_ip)))
		return false;
	if (return_ip < 2 || !sas_user_ip(return_ip) ||
	    !sas_user_ip(return_ip - 2))
		return false;
	if (copy_from_user_nofault(&instruction,
				   (void __user *)(return_ip - 2),
				   sizeof(instruction)))
		return false;
	if (instruction != 0xd0ff)
		return false;

	zpoline_set_invalid_syscall_return(mc, return_ip, sp);
	return true;
}
#endif
