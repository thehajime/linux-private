/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
#ifndef SWMMU_DIRECT_WINDOW_GUARD_H
#define SWMMU_DIRECT_WINDOW_GUARD_H

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
#if UINTPTR_MAX != UINT64_MAX
#error "SWMMU direct-window guards require 64-bit pointers"
#endif

extern uint64_t nommu_swmmu_load_dynamic(const void *, size_t);
extern long nommu_swmmu_store_dynamic(void *, size_t, uint64_t);

typedef uint8_t swmmu_alias_u8 __attribute__((may_alias, aligned(1)));
typedef uint16_t swmmu_alias_u16 __attribute__((may_alias, aligned(1)));
typedef uint32_t swmmu_alias_u32 __attribute__((may_alias, aligned(1)));
typedef uint64_t swmmu_alias_u64 __attribute__((may_alias, aligned(1)));

typedef const void * __attribute__((swmmu_native_ptr)) swmmu_direct_const_ptr;
typedef void * __attribute__((swmmu_native_ptr)) swmmu_direct_ptr;

/* Compiler-only const queries are lowered immediately before RTL expansion.
 * This lets scalar optimization reuse the authorization snapshot without a
 * runtime call. A cached disabled snapshot only loses the fast path.
 * Valid keys have prefix >= 2 and shift >= 12; mixed cached sentinel/valid
 * prefix and shift fields also fail closed. No mutable VMA permission is
 * cached. The late read does not publish user data or order its accesses.
 */
extern uintptr_t __swmmu_direct_key_snapshot(void) __attribute__((const, nothrow));
extern uintptr_t __swmmu_direct_key_snapshot_libc(void) __attribute__((const, nothrow));
#if defined(SWMMU_BUILDING_LIBC) && (!defined(CRT) || defined(SWMMU_STATIC_PIE_CRT))
#define SWMMU_READ_DIRECT_KEY(result) ((result) = __swmmu_direct_key_snapshot_libc())
#else
#define SWMMU_READ_DIRECT_KEY(result) ((result) = __swmmu_direct_key_snapshot())
#endif

static inline __attribute__((always_inline, used)) uint64_t
nommu_swmmu_load_dynamic_guarded(swmmu_direct_const_ptr p, size_t size)
{
	uintptr_t key;
	SWMMU_READ_DIRECT_KEY(key);
	uintptr_t address = (uintptr_t)p;
	uint64_t value;

	if ((address >> (key & 63)) != (key >> 6) ||
	    size > 4096 - (address & 4095))
		return nommu_swmmu_load_dynamic(p, size);
	switch (size) {
	case 1: {
		uint8_t v;
		__asm__("movb %1, %0" : "=q"(v) : "m"(*(const swmmu_alias_u8 *)p));
		return v;
	}
	case 2: {
		uint16_t v;
		__asm__("movw %1, %0" : "=r"(v) : "m"(*(const swmmu_alias_u16 *)p));
		return v;
	}
	case 4: {
		uint32_t v;
		__asm__("movl %1, %0" : "=r"(v) : "m"(*(const swmmu_alias_u32 *)p));
		return v;
	}
	case 8:
		__asm__("movq %1, %0" : "=r"(value) : "m"(*(const swmmu_alias_u64 *)p));
		return value;
	default:
		return nommu_swmmu_load_dynamic(p, size);
	}
}

/* A naturally aligned power-of-two scalar cannot cross a page boundary. */
static inline __attribute__((always_inline, used)) uint64_t
nommu_swmmu_load_dynamic_guarded_aligned(swmmu_direct_const_ptr p, size_t size)
{
	uintptr_t key;
	SWMMU_READ_DIRECT_KEY(key);
	uintptr_t address = (uintptr_t)p;
	uint64_t value;

	if ((address >> (key & 63)) != (key >> 6))
		return nommu_swmmu_load_dynamic(p, size);
	switch (size) {
	case 1: {
		uint8_t v;
		__asm__("movb %1, %0" : "=q"(v) : "m"(*(const swmmu_alias_u8 *)p));
		return v;
	}
	case 2: {
		uint16_t v;
		__asm__("movw %1, %0" : "=r"(v) : "m"(*(const swmmu_alias_u16 *)p));
		return v;
	}
	case 4: {
		uint32_t v;
		__asm__("movl %1, %0" : "=r"(v) : "m"(*(const swmmu_alias_u32 *)p));
		return v;
	}
	case 8:
		__asm__("movq %1, %0" : "=r"(value) : "m"(*(const swmmu_alias_u64 *)p));
		return value;
	default:
		return nommu_swmmu_load_dynamic(p, size);
	}
}

static inline __attribute__((always_inline, used)) long
nommu_swmmu_store_dynamic_guarded(swmmu_direct_ptr p, size_t size, uint64_t value)
{
	uintptr_t key;
	SWMMU_READ_DIRECT_KEY(key);
	uintptr_t address = (uintptr_t)p;

	if ((address >> (key & 63)) != (key >> 6) ||
	    size > 4096 - (address & 4095))
		return nommu_swmmu_store_dynamic(p, size, value);
	switch (size) {
	case 1: {
		uint8_t v = value;
		__asm__("movb %1, %0" : "=m"(*(swmmu_alias_u8 *)p) : "q"(v));
		break;
	}
	case 2: {
		uint16_t v = value;
		__asm__("movw %1, %0" : "=m"(*(swmmu_alias_u16 *)p) : "r"(v));
		break;
	}
	case 4: {
		uint32_t v = value;
		__asm__("movl %1, %0" : "=m"(*(swmmu_alias_u32 *)p) : "r"(v));
		break;
	}
	case 8:
		__asm__("movq %1, %0" : "=m"(*(swmmu_alias_u64 *)p) : "r"(value));
		break;
	default:
		return nommu_swmmu_store_dynamic(p, size, value);
	}
	return 0;
}

static inline __attribute__((always_inline, used)) long
nommu_swmmu_store_dynamic_guarded_aligned(swmmu_direct_ptr p, size_t size,
					   uint64_t value)
{
	uintptr_t key;
	SWMMU_READ_DIRECT_KEY(key);
	uintptr_t address = (uintptr_t)p;

	if ((address >> (key & 63)) != (key >> 6))
		return nommu_swmmu_store_dynamic(p, size, value);
	switch (size) {
	case 1: {
		uint8_t v = value;
		__asm__("movb %1, %0" : "=m"(*(swmmu_alias_u8 *)p) : "q"(v));
		break;
	}
	case 2: {
		uint16_t v = value;
		__asm__("movw %1, %0" : "=m"(*(swmmu_alias_u16 *)p) : "r"(v));
		break;
	}
	case 4: {
		uint32_t v = value;
		__asm__("movl %1, %0" : "=m"(*(swmmu_alias_u32 *)p) : "r"(v));
		break;
	}
	case 8:
		__asm__("movq %1, %0" : "=m"(*(swmmu_alias_u64 *)p) : "r"(value));
		break;
	default:
		return nommu_swmmu_store_dynamic(p, size, value);
	}
	return 0;
}
#endif
#endif
