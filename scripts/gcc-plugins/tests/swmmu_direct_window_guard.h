/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SWMMU_DIRECT_WINDOW_GUARD_H
#define SWMMU_DIRECT_WINDOW_GUARD_H

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

#ifdef SWMMU_BUILDING_LIBC
extern uintptr_t __nommu_swmmu_direct_start __attribute__((visibility("hidden")));
extern uintptr_t __nommu_swmmu_direct_end __attribute__((visibility("hidden")));
#define SWMMU_DIRECT_START __nommu_swmmu_direct_start
#define SWMMU_DIRECT_END __nommu_swmmu_direct_end
#else
extern uintptr_t nommu_swmmu_direct_start;
extern uintptr_t nommu_swmmu_direct_end;
#define SWMMU_DIRECT_START nommu_swmmu_direct_start
#define SWMMU_DIRECT_END nommu_swmmu_direct_end
#endif

extern uint64_t nommu_swmmu_load_dynamic(const void *, size_t);
extern long nommu_swmmu_store_dynamic(void *, size_t, uint64_t);

typedef const void * __attribute__((swmmu_native_ptr)) swmmu_direct_const_ptr;
typedef void * __attribute__((swmmu_native_ptr)) swmmu_direct_ptr;

/* End is published after start, once; zero bounds force runtime fallback. */
static inline __attribute__((always_inline, used)) uint64_t
nommu_swmmu_load_dynamic_guarded(swmmu_direct_const_ptr p, size_t size)
{
	uintptr_t end = __atomic_load_n(&SWMMU_DIRECT_END, __ATOMIC_ACQUIRE);
	uintptr_t start = __atomic_load_n(&SWMMU_DIRECT_START, __ATOMIC_RELAXED);
	uintptr_t address = (uintptr_t)p;
	uint64_t value;

	if (!end || address < start || address >= end || size > end - address ||
	    size > 4096 - (address & 4095))
		return nommu_swmmu_load_dynamic(p, size);
	switch (size) {
	case 1: {
		uint8_t v;
		__asm__ volatile("movb (%1), %0" : "=q"(v) : "r"(p) : "memory");
		return v;
	}
	case 2: {
		uint16_t v;
		__asm__ volatile("movw (%1), %0" : "=r"(v) : "r"(p) : "memory");
		return v;
	}
	case 4: {
		uint32_t v;
		__asm__ volatile("movl (%1), %0" : "=r"(v) : "r"(p) : "memory");
		return v;
	}
	case 8:
		__asm__ volatile("movq (%1), %0" : "=r"(value) : "r"(p) : "memory");
		return value;
	default:
		return nommu_swmmu_load_dynamic(p, size);
	}
}

static inline __attribute__((always_inline, used)) long
nommu_swmmu_store_dynamic_guarded(swmmu_direct_ptr p, size_t size, uint64_t value)
{
	uintptr_t end = __atomic_load_n(&SWMMU_DIRECT_END, __ATOMIC_ACQUIRE);
	uintptr_t start = __atomic_load_n(&SWMMU_DIRECT_START, __ATOMIC_RELAXED);
	uintptr_t address = (uintptr_t)p;

	if (!end || address < start || address >= end || size > end - address ||
	    size > 4096 - (address & 4095))
		return nommu_swmmu_store_dynamic(p, size, value);
	switch (size) {
	case 1: {
		uint8_t v = value;
		__asm__ volatile("movb %1, (%0)" : : "r"(p), "q"(v) : "memory");
		break;
	}
	case 2: {
		uint16_t v = value;
		__asm__ volatile("movw %1, (%0)" : : "r"(p), "r"(v) : "memory");
		break;
	}
	case 4: {
		uint32_t v = value;
		__asm__ volatile("movl %1, (%0)" : : "r"(p), "r"(v) : "memory");
		break;
	}
	case 8:
		__asm__ volatile("movq %1, (%0)" : : "r"(p), "r"(value) : "memory");
		break;
	default:
		return nommu_swmmu_store_dynamic(p, size, value);
	}
	return 0;
}
#endif
#endif
