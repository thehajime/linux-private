/* SPDX-License-Identifier: GPL-2.0 */
/* Host-side lowering test: token pointers must never be dereferenced directly. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TOKEN_BASE ((uintptr_t)0x100000000000ULL)
static uint64_t backing[32];
static unsigned long loads, stores;
uintptr_t nommu_swmmu_direct_start, nommu_swmmu_direct_end;

extern uint64_t swmmu_test_sum(const uint64_t *, size_t);
extern uint64_t swmmu_test_invariant(const uint64_t *, size_t);
extern uint64_t swmmu_test_volatile(const volatile uint64_t *);
extern uint64_t swmmu_test_call(const uint64_t *);
extern uint64_t swmmu_test_alias(uint64_t *, uint64_t *);
extern uint64_t swmmu_test_o0(const uint64_t *);

static void *resolve(const void *p, size_t size)
{
	uintptr_t a = (uintptr_t)p;

	if (a >= TOKEN_BASE && a - TOKEN_BASE < sizeof(backing)) {
		assert(size <= sizeof(backing) - (a - TOKEN_BASE));
		return (char *)backing + (a - TOKEN_BASE);
	}
	return (void *)p;
}

uint64_t nommu_swmmu_load_dynamic(const void *p, size_t size)
{
	uint64_t value = 0;

	assert(size <= sizeof(value));
	loads++;
	memcpy(&value, resolve(p, size), size);
	return value;
}

long nommu_swmmu_store_dynamic(void *p, size_t size, uint64_t value)
{
	assert(size <= sizeof(value));
	stores++;
	memcpy(resolve(p, size), &value, size);
	return 0;
}

void swmmu_test_barrier(void)
{
	backing[0] = 11;
}

int main(void)
{
	uint64_t *p = (uint64_t *)TOKEN_BASE;
	unsigned long before;

	for (unsigned int i = 0; i < 32; i++)
		backing[i] = i + 1;
	assert(swmmu_test_sum(p, 32) == 528);
	before = loads;
	assert(swmmu_test_invariant(p, 32) == 32);
	printf("invariant_loads=%lu\n", loads - before);
	before = loads;
	assert(swmmu_test_volatile(p) == 2);
	assert(loads - before == 2);
	assert(swmmu_test_call(p) == 12);
	assert(swmmu_test_alias(p, p) == 7);
	assert(swmmu_test_alias(p, p + 1) == 3);
	assert(swmmu_test_o0(p) == 3);
#ifdef SWMMU_TEST_DIRECT_WINDOW
	/* Only real backing is direct; token pointers must still use helpers. */
	__atomic_store_n(&nommu_swmmu_direct_start, (uintptr_t)backing,
			__ATOMIC_RELAXED);
	__atomic_store_n(&nommu_swmmu_direct_end,
			(uintptr_t)backing + sizeof(backing), __ATOMIC_RELEASE);
	before = loads;
	assert(swmmu_test_sum(backing, 1) == 3);
	assert(loads == before);
	before = stores;
	assert(swmmu_test_alias(backing, backing + 1) == 3);
	assert(stores == before);
	puts("PASS published direct window avoids helpers");
#endif
	printf("PASS token pointers, volatile, call barrier, aliases, O0; loads=%lu stores=%lu\n",
	       loads, stores);
	return 0;
}
