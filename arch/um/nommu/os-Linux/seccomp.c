// SPDX-License-Identifier: GPL-2.0

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include <init.h>
#include <as-layout.h>
#include <os.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

extern unsigned long task_size;

static int __init os_install_seccomp_ip_filter(unsigned long userspace_start,
					       unsigned long userspace_end)
{
	struct sock_filter filter[] = {
		/* if (IP_high > __userspace_end) allow; */
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP + BPF_JGT + BPF_K, userspace_end >> 32,
			 /*true-skip=*/0, /*false-skip=*/1),
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

		/* if (IP_high == __userspace_end && IP_low >= __userspace_end) allow; */
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, userspace_end >> 32,
			 /*true-skip=*/0, /*false-skip=*/3),
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer)),
		BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, userspace_end,
			 /*true-skip=*/0, /*false-skip=*/1),
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

		/* if (IP_high < __userspace_start) allow; */
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, userspace_start >> 32,
			 /*true-skip=*/1, /*false-skip=*/0),
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

		/* if (IP_high == __userspace_start && IP_low < __userspace_start) allow; */
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, userspace_start >> 32,
			 /*true-skip=*/0, /*false-skip=*/3),
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer)),
		BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, userspace_start,
			 /*true-skip=*/1, /*false-skip=*/0),
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

		/* other address; trap  */
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRAP),
	};
	struct sock_fprog prog = {
		.len = ARRAY_SIZE(filter),
		.filter = filter,
	};

	return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
		       SECCOMP_FILTER_FLAG_TSYNC, &prog);
}

int __init os_setup_seccomp(void)
{
	int err;
#ifndef CONFIG_UML_NOMMU_SAS
	unsigned long userspace_start = uml_reserved;
	unsigned long userspace_end = high_physmem;
#else
	unsigned long native_start = uml_reserved + 1;
	unsigned long native_end = high_physmem;
	unsigned long swmmu_start = UM_NOMMU_SWMMU_VA_BASE;
	unsigned long swmmu_end = task_size;
#endif

	err = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	if (err)
		os_warn("PR_SET_NO_NEW_PRIVS (err=%d, ernro=%d)\n",
		       err, errno);

#ifndef CONFIG_UML_NOMMU_SAS
	err = os_install_seccomp_ip_filter(userspace_start, userspace_end);
#else
	/*
	 * Stack one filter for each disjoint user-IP range. Seccomp evaluates
	 * all stacked filters, so the two TRAP intervals match nommu_user_ip().
	 */
	err = os_install_seccomp_ip_filter(native_start, native_end);
#endif
	if (err) {
		os_warn("SECCOMP_SET_MODE_FILTER (err=%d, ernro=%d)\n",
		       err, errno);
		exit(1);
	}

#ifdef CONFIG_UML_NOMMU_SAS
	err = os_install_seccomp_ip_filter(swmmu_start, swmmu_end);
	if (err) {
		os_warn("SECCOMP_SET_MODE_FILTER (err=%d, errno=%d)\n",
			err, errno);
		exit(1);
	}
#endif

	set_handler(SIGSYS);

#ifndef CONFIG_UML_NOMMU_SAS
	os_info("seccomp: setup filter syscalls in the range: 0x%lx-0x%lx\n",
		userspace_start, userspace_end);
#else
	os_info("seccomp: setup native filter in the range: 0x%lx-0x%lx\n",
		native_start, native_end);
	os_info("seccomp: setup SWMMU filter in the range: 0x%lx-0x%lx\n",
		swmmu_start, swmmu_end);
#endif

	return 0;
}
