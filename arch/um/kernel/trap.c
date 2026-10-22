// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/nommu_swmmu.h>
#include <linux/sched/signal.h>
#include <linux/hardirq.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/sched/debug.h>
#include <asm/current.h>
#include <asm/unistd.h>
#include <asm/tlbflush.h>
#include <arch.h>
#include <as-layout.h>
#include <kern_util.h>
#include <os.h>
#include <skas.h>

static void show_segv_info(struct uml_pt_regs *regs)
{
	struct task_struct *tsk = current;
	struct faultinfo *fi = UPT_FAULTINFO(regs);

	if (!unhandled_signal(tsk, SIGSEGV))
		return;

	if (!printk_ratelimit())
		return;

	printk("%s%s[%d]: segfault at %lx ip %px sp %px error %x",
		task_pid_nr(tsk) > 1 ? KERN_INFO : KERN_EMERG,
		tsk->comm, task_pid_nr(tsk), FAULT_ADDRESS(*fi),
		(void *)UPT_IP(regs), (void *)UPT_SP(regs),
		fi->error_code);

	print_vma_addr(KERN_CONT " in ", UPT_IP(regs));
	printk(KERN_CONT "\n");
}

#ifndef CONFIG_UML_NOMMU_SAS
static void bad_segv(struct faultinfo fi, unsigned long ip)
{
	current->thread.arch.faultinfo = fi;
	force_sig_fault(SIGSEGV, SEGV_ACCERR, (void __user *) FAULT_ADDRESS(fi));
}
#endif

void fatal_sigsegv(void)
{
	force_fatal_sig(SIGSEGV);
	do_signal(&current->thread.regs);
	/*
	 * This is to tell gcc that we're not returning - do_signal
	 * can, in general, return, but in this case, it's not, since
	 * we just got a fatal SIGSEGV queued.
	 */
	os_dump_core();
}

#ifdef CONFIG_UML_NOMMU_SAS
static bool nommu_user_ip(unsigned long ip)
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
static bool nommu_zpoline_invalid_syscall(struct uml_pt_regs *regs,
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
	    !nommu_user_ip(sp) || !nommu_user_ip(sp + sizeof(return_ip) - 1))
		return false;
	if (copy_from_user_nofault(&return_ip, (void __user *)sp,
				   sizeof(return_ip)))
		return false;
	if (return_ip < 2 || !nommu_user_ip(return_ip) ||
	    !nommu_user_ip(return_ip - 2))
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
#endif

/**
 * segv_handler() - the SIGSEGV handler
 * @sig:	the signal number
 * @unused_si:	the signal info struct; unused in this handler
 * @regs:	the ptrace register information
 * @mc:		the mcontext of the signal
 *
 * The handler first extracts the faultinfo from the UML ptrace regs struct.
 * If the userfault did not happen in an UML userspace process, bad_segv is called.
 * Otherwise the signal did happen in a cloned userspace process, handle it.
 */
void segv_handler(int sig, struct siginfo *unused_si, struct uml_pt_regs *regs,
		  void *mc)
{
	struct faultinfo * fi = UPT_FAULTINFO(regs);

#ifdef CONFIG_UML_NOMMU_SAS
#ifdef CONFIG_64BIT
	if (nommu_zpoline_invalid_syscall(regs, unused_si, mc))
		return;
#endif

	/* SWMMU user instructions may live above the normal NOMMU range. */
	if (nommu_user_ip(UPT_IP(regs)))
		regs->is_user = 1;
#endif

	if (UPT_IS_USER(regs) && !SEGV_IS_FIXABLE(fi)) {
		show_segv_info(regs);
#ifdef CONFIG_UML_NOMMU_SAS
		/*
		 * This host fault cannot be fixed by UML. Relay it through the
		 * NOMMU userspace trampoline so the guest process receives it.
		 * Returning directly leaves the host context at the faulting
		 * instruction and repeats the same SIGSEGV indefinitely.
		 */
		relay_signal(sig, unused_si, regs, mc);
#else
		bad_segv(*fi, UPT_IP(regs));
#endif
		return;
	}
	if (segv(*fi, UPT_IP(regs), UPT_IS_USER(regs), regs, mc) > 0) {
#ifdef CONFIG_UML_NOMMU_SAS
		/* Retry a repaired alias through the normal userspace entry path. */
		nommu_relay_signal(mc);
#endif
		return;
	}

#ifdef CONFIG_UML_NOMMU_SAS
	/* NOMMU must relay the fault through its userspace signal trampoline. */
	relay_signal(sig, unused_si, regs, mc);
#endif
}

/*
 * We give a *copy* of the faultinfo in the regs to segv.
 * This must be done, since nesting SEGVs could overwrite
 * the info in the regs. A pointer to the info then would
 * give us bad data!
 */
unsigned long segv(struct faultinfo fi, unsigned long ip, int is_user,
		   struct uml_pt_regs *regs, void *mc)
{
	int si_code;
	int err;
	bool alias_recovered = false;
	int is_write = FAULT_WRITE(fi);
	unsigned long address = FAULT_ADDRESS(fi);

	if (!is_user && regs)
		current->thread.segv_regs = container_of(regs, struct pt_regs, regs);

	if (!is_user && address >= start_vm && address < end_vm) {
		/*
		 * Kernel has pending updates from set_ptes that were not
		 * flushed yet. Syncing them should fix the pagefault (if not
		 * we'll get here again and panic).
		 */
		err = um_tlb_sync(&init_mm);
		if (err == -ENOMEM)
			report_enomem();
		if (err)
			panic("Failed to sync kernel TLBs: %d", err);
		goto out;
	}
	else if (current->pagefault_disabled) {
		if (!mc) {
			show_regs(container_of(regs, struct pt_regs, regs));
			panic("Segfault with pagefaults disabled but no mcontext");
		}
		if (!current->thread.segv_continue) {
			show_regs(container_of(regs, struct pt_regs, regs));
			panic("Segfault without recovery target");
		}
		mc_set_rip(mc, current->thread.segv_continue);
		current->thread.segv_continue = NULL;
		goto out;
	}
	else if (current->mm == NULL) {
		show_regs(container_of(regs, struct pt_regs, regs));
		panic("Segfault with no mm");
	}
	else if (!is_user && address > PAGE_SIZE && address < TASK_SIZE) {
		show_regs(container_of(regs, struct pt_regs, regs));
		panic("Kernel tried to access user memory at addr 0x%lx, ip 0x%lx",
		       address, ip);
	}

#ifdef CONFIG_NOMMU_SWMMU
	/* Retry a user instruction after restoring a displaced host alias. */
	if (is_user) {
		unsigned int access = fi.error_code & 0x10 ? PROT_EXEC :
			(is_write ? PROT_WRITE : PROT_READ);

		err = nommu_swmmu_recover_alias_fault(current->mm,
						 address, access);
		if (err > 0) {
			alias_recovered = true;
			goto out;
		}

		/* Invalid CR2 can result from executing a displaced code alias. */
		if (ip != address) {
			err = nommu_swmmu_recover_alias_fault(current->mm, ip,
							     PROT_EXEC);
			if (err > 0) {
				alias_recovered = true;
				goto out;
			}
		}
	}
#endif

	if (SEGV_IS_FIXABLE(&fi))
		err = handle_page_fault(address, ip, is_write, is_user,
					&si_code);
	else {
		err = -EFAULT;
		/*
		 * A thread accessed NULL, we get a fault, but CR2 is invalid.
		 * This code is used in __do_copy_from_user() of TT mode.
		 * XXX tt mode is gone, so maybe this isn't needed any more
		 */
		address = 0;
	}

	if (!err)
		goto out;
	else if (!is_user && arch_fixup(ip, regs))
		goto out;

	if (!is_user) {
		show_regs(container_of(regs, struct pt_regs, regs));
		panic("Kernel mode fault at addr 0x%lx, ip 0x%lx",
		      address, ip);
	}

	show_segv_info(regs);

	if (err == -EACCES) {
		current->thread.arch.faultinfo = fi;
		force_sig_fault(SIGBUS, BUS_ADRERR, (void __user *)address);
	} else {
		BUG_ON(err != -EFAULT);
		current->thread.arch.faultinfo = fi;
		force_sig_fault(SIGSEGV, si_code, (void __user *) address);
	}

out:
	if (regs)
		current->thread.segv_regs = NULL;

	return alias_recovered;
}

void relay_signal(int sig, struct siginfo *si, struct uml_pt_regs *regs,
		  void *mc)
{
	int code, err;

#ifdef CONFIG_UML_NOMMU_SAS
	/* !MMU specific part; detection of userspace */
	/* mark is_user=1 when the IP is from userspace code. */
	if (nommu_user_ip(UPT_IP(regs)))
		regs->is_user = 1;
#endif

	if (!UPT_IS_USER(regs)) {
		if (sig == SIGBUS)
			printk(KERN_ERR "Bus error - the host /dev/shm or /tmp "
			       "mount likely just ran out of space\n");
		panic("Kernel mode signal %d", sig);
	}

#ifdef CONFIG_NOMMU_SWMMU
	/* Retry SIGILL if a displaced code alias has just been restored. */
	if (sig == SIGILL &&
	    nommu_swmmu_recover_alias_fault(current->mm, UPT_IP(regs),
					    PROT_EXEC) > 0) {
#ifdef CONFIG_UML_NOMMU_SAS
		nommu_relay_signal(mc);
#endif
		return;
	}
#endif

	/* if is_user==1, set return to userspace sig handler to relay signal */
#ifdef CONFIG_UML_NOMMU_SAS
	nommu_relay_signal(mc);
#endif

	arch_examine_signal(sig, regs);

	/* Is the signal layout for the signal known?
	 * Signal data must be scrubbed to prevent information leaks.
	 */
	code = si->si_code;
	err = si->si_errno;
	if ((err == 0) && (siginfo_layout(sig, code) == SIL_FAULT)) {
		struct faultinfo *fi = UPT_FAULTINFO(regs);
		current->thread.arch.faultinfo = *fi;
		force_sig_fault(sig, code, (void __user *)FAULT_ADDRESS(*fi));
	} else {
		printk(KERN_ERR "Attempted to relay unknown signal %d (si_code = %d) with errno %d\n",
		       sig, code, err);
		force_sig(sig);
	}
}

void winch(int sig, struct siginfo *unused_si, struct uml_pt_regs *regs,
	   void *mc)
{
	do_IRQ(WINCH_IRQ, regs);
}
