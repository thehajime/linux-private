// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/mm.h>
#include <as-layout.h>
#include <shared/kern_util.h>

int handle_page_fault(unsigned long address, unsigned long ip,
		      int is_write, int is_user, int *code_out)
{
	/* everything that's valid is already mapped in NOMMU */
	*code_out = SEGV_MAPERR;
#if defined(CONFIG_NOMMU_SWMMU) && defined(CONFIG_UML_NOMMU_SAS)
	/*
	 * SAS with SWMMU reserves this direct window for userspace aliases.
	 * A fault in an existing VMA there is a protection fault; ordinary
	 * NOMMU faults keep the default SEGV_MAPERR classification above.
	 */
	if (is_user && address >= um_nommu_swmmu_direct_start &&
	    address < um_nommu_swmmu_direct_end && current->mm) {
		struct vm_area_struct *vma;

		mmap_read_lock(current->mm);
		vma = find_vma(current->mm, address);
		if (vma && address >= vma->vm_start)
			*code_out = SEGV_ACCERR;
		mmap_read_unlock(current->mm);
	}
#endif
	return -EFAULT;
}
