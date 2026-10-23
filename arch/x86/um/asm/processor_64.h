/*
 * Copyright 2003 PathScale, Inc.
 *
 * Licensed under the GPL
 */

#ifndef __UM_PROCESSOR_X86_64_H
#define __UM_PROCESSOR_X86_64_H

struct uml_nommu_user_stack;
struct arch_thread {
        unsigned long debugregs[8];
        int debugregs_seq;
        struct faultinfo faultinfo;
#ifdef CONFIG_NOMMU_SWMMU
	struct uml_nommu_user_stack *nommu_user_stack;
#endif
};

#define INIT_ARCH_THREAD { .debugregs  		= { [ 0 ... 7 ] = 0 }, \
			   .debugregs_seq	= 0, \
			   .faultinfo		= { 0, 0, 0 } }

#ifdef CONFIG_MMU
/* MAP_FIXED ignores MAP_32BIT, matching the x86-64 mmap ABI. */
#define arch_get_mmap_end(addr, len, flags) \
	((((flags) & MAP_32BIT) && !((flags) & MAP_FIXED)) ? \
	 min(TASK_SIZE, 1UL << 31) : TASK_SIZE)
#endif

#define STACKSLOTS_PER_LINE 4

static inline void arch_flush_thread(struct arch_thread *thread)
{
}

static inline void arch_copy_thread(struct arch_thread *from,
                                    struct arch_thread *to)
{
}

#define current_sp() ({ void *sp; __asm__("movq %%rsp, %0" : "=r" (sp) : ); sp; })
#define current_bp() ({ unsigned long bp; __asm__("movq %%rbp, %0" : "=r" (bp) : ); bp; })

#endif
