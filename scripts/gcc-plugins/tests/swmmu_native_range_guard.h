#ifndef SWMMU_NATIVE_RANGE_GUARD_H
#define SWMMU_NATIVE_RANGE_GUARD_H

#include <stddef.h>
#include <stdint.h>

extern uint64_t nommu_swmmu_load_dynamic(const void *, size_t);
extern long nommu_swmmu_store_dynamic(void *, size_t, uint64_t);

#ifndef SWMMU_NATIVE_VA_BASE
#error SWMMU_NATIVE_VA_BASE must match the kernel SWMMU VA base
#endif

typedef const void * __attribute__((swmmu_native_ptr)) swmmu_guarded_const_ptr;
typedef void * __attribute__((swmmu_native_ptr)) swmmu_guarded_ptr;

static inline __attribute__((used, always_inline)) uint64_t
nommu_swmmu_load_dynamic_guarded(swmmu_guarded_const_ptr address, size_t size)
{
	uintptr_t addr = (uintptr_t)address;

	if (addr >= 4096 && addr < SWMMU_NATIVE_VA_BASE &&
	    size <= SWMMU_NATIVE_VA_BASE - addr &&
	    (size == 1 || size == 2 || size == 4 || size == 8)) {
		switch (size) {
		case 1: {
			uint8_t value;

			__asm__ volatile("movb (%1), %0"
					 : "=q"(value) : "r"(address) : "memory");
			return value;
		}
		case 2: {
			uint16_t value;

			__asm__ volatile("movw (%1), %0"
					 : "=r"(value) : "r"(address) : "memory");
			return value;
		}
		case 4: {
			uint32_t value;

			__asm__ volatile("movl (%1), %0"
					 : "=r"(value) : "r"(address) : "memory");
			return value;
		}
		default: {
			uint64_t value;

			__asm__ volatile("movq (%1), %0"
					 : "=r"(value) : "r"(address) : "memory");
			return value;
		}
		}
	}

	return nommu_swmmu_load_dynamic(address, size);
}

static inline __attribute__((used, always_inline)) long
nommu_swmmu_store_dynamic_guarded(swmmu_guarded_ptr address, size_t size,
				  uint64_t value)
{
	uintptr_t addr = (uintptr_t)address;

	if (addr >= 4096 && addr < SWMMU_NATIVE_VA_BASE &&
	    size <= SWMMU_NATIVE_VA_BASE - addr &&
	    (size == 1 || size == 2 || size == 4 || size == 8)) {
		switch (size) {
		case 1: {
			uint8_t part = value;

			__asm__ volatile("movb %1, (%0)" ::
					 "r"(address), "q"(part) : "memory");
			break;
		}
		case 2: {
			uint16_t part = value;

			__asm__ volatile("movw %1, (%0)" ::
					 "r"(address), "r"(part) : "memory");
			break;
		}
		case 4: {
			uint32_t part = value;

			__asm__ volatile("movl %1, (%0)" ::
					 "r"(address), "r"(part) : "memory");
			break;
		}
		default:
			__asm__ volatile("movq %1, (%0)" ::
					 "r"(address), "r"(value) : "memory");
			break;
		}
		return 0;
	}

	return nommu_swmmu_store_dynamic(address, size, value);
}

#endif
