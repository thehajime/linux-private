/* SPDX-License-Identifier: GPL-2.0 */
/* Host-side lowering test: token pointers must never be dereferenced directly. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TOKEN_BASE ((uintptr_t)0x100000000000ULL)
static uint64_t backing[1024] __attribute__((aligned(4096)));
static unsigned long loads, stores;
extern volatile uint64_t swmmu_test_global __attribute__((alias("backing")));

uintptr_t nommu_swmmu_direct_key = UINTPTR_MAX;

void swmmu_test_publish_window(void)
{
	backing[0] = 11;
	__atomic_store_n(&nommu_swmmu_direct_key,
			(((uintptr_t)backing >> 12) << 6) | 12, __ATOMIC_RELEASE);
}

extern uint64_t swmmu_test_sum(const uint64_t *, size_t);
extern uint64_t swmmu_test_invariant(const uint64_t *, size_t);
extern uint64_t swmmu_test_volatile(const volatile uint64_t *);
extern void swmmu_test_volatile_store(volatile uint64_t *);
extern uint64_t swmmu_test_call(const uint64_t *);
extern uint64_t swmmu_test_alias(uint64_t *, uint64_t *);
extern uint64_t swmmu_test_o0(const uint64_t *);
extern volatile uint64_t swmmu_test_global;
extern uint64_t swmmu_test_global_load(void);
extern void swmmu_test_global_store(void);
extern uint64_t swmmu_test_unaligned_load(const void *);
extern void swmmu_test_unaligned_store(void *, uint64_t);
extern uint64_t swmmu_test_window_transition(const uint64_t *);

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
	before = stores;
	swmmu_test_volatile_store(p);
	assert(stores - before == 2);
#if defined(SWMMU_TEST_DIRECT_WINDOW) && !defined(SWMMU_TEST_DISABLED_WINDOW)
	/* A stale disabled snapshot remains correct after publication. */
	before = loads;
	assert(swmmu_test_window_transition(backing) == 13);
	assert(loads - before >= 1 && loads - before <= 2);
	backing[0] = 2;
	/* Only real backing is direct; token pointers must still use helpers. */
	__atomic_store_n(&nommu_swmmu_direct_key,
			(((uintptr_t)backing >> 12) << 6) | 12, __ATOMIC_RELEASE);
	before = loads;
	assert(swmmu_test_sum(backing, 1) == 2);
	assert(loads == before);
	assert(swmmu_test_sum(p, 1) == 2);
	assert(loads == before + 1);
	before = loads;
	assert(swmmu_test_call(backing) == 13);
	assert(loads == before);
	before = loads;
	assert(swmmu_test_sum((void *)0x1000, 0) == 0);
	assert(loads == before);
	before = stores;
	assert(swmmu_test_alias(backing, backing + 1) == 3);
	assert(stores == before);
	before = loads;
	assert(swmmu_test_volatile(backing) == 6);
	assert(loads - before == 2);
	before = stores;
	swmmu_test_volatile_store(backing);
	assert(stores - before == 2);
	/* The key covers one page, even when the queried bounds are larger. */
	backing[512] = 55;
	before = loads;
	assert(swmmu_test_sum(backing + 512, 1) == 55);
	assert(loads - before == 1);
	/* Starting in the key is insufficient when an access crosses a page. */
	void *crossing = (char *)backing + 4093;
	uint64_t pattern = UINT64_C(0x8877665544332211), observed;
	memcpy(crossing, &pattern, sizeof(pattern));
	before = loads;
	assert(swmmu_test_unaligned_load(crossing) == pattern);
	assert(loads - before == 1);
	before = stores;
	swmmu_test_unaligned_store(crossing, ~pattern);
	assert(stores - before == 1);
	memcpy(&observed, crossing, sizeof(observed));
	assert(observed == ~pattern);
	/* The global aliases backing[0], inside the same fixed test window. */
	before = stores;
	swmmu_test_global_store();
	assert(stores - before == 2);
	before = loads;
	assert(swmmu_test_global_load() == 4);
	assert(loads - before == 2);
	puts("PASS published direct window avoids helpers");
#elif defined(SWMMU_TEST_DISABLED_WINDOW)
	before = loads;
	assert(swmmu_test_sum(backing, 1) == 2);
	assert(loads - before == 1);
	puts("PASS disabled window retains helper fallback");
#endif
	printf("PASS token pointers, volatile, call barrier, aliases, O0; loads=%lu stores=%lu\n",
	       loads, stores);
	return 0;
}
