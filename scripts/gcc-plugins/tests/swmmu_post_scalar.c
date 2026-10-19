/* SPDX-License-Identifier: GPL-2.0 */
#include "swmmu_test_runtime.h"

extern void swmmu_test_barrier(void);
extern void swmmu_test_publish_window(void);
extern volatile uint64_t swmmu_test_global;

__attribute__((noinline, noipa))
uint64_t swmmu_test_window_transition(const uint64_t *p)
{
	uint64_t before = *p;

	swmmu_test_publish_window();
	return before + *p;
}

struct __attribute__((packed)) swmmu_test_packed {
	uint64_t value;
};

__attribute__((noinline, noipa))
uint64_t swmmu_test_unaligned_load(const struct swmmu_test_packed *p)
{
	return p->value;
}

__attribute__((noinline, noipa))
void swmmu_test_unaligned_store(struct swmmu_test_packed *p, uint64_t value)
{
	p->value = value;
}

__attribute__((noinline, noipa))
uint64_t swmmu_test_global_load(void)
{
	return swmmu_test_global + swmmu_test_global;
}

__attribute__((noinline, noipa))
void swmmu_test_global_store(void)
{
	swmmu_test_global = 1;
	swmmu_test_global = 2;
}

__attribute__((noinline, noipa))
uint64_t swmmu_test_invariant(const uint64_t *p, size_t n)
{
	uint64_t sum = 0;

	for (size_t i = 0; i < n; i++)
		sum += *p;
	return sum;
}

__attribute__((noinline, noipa))
uint64_t swmmu_test_sum(const uint64_t *p, size_t n)
{
	uint64_t sum = 0;

	for (size_t i = 0; i < n; i++)
		sum += p[i];
	return sum;
}

__attribute__((noinline, noipa))
uint64_t swmmu_test_volatile(const volatile uint64_t *p)
{
	return *p + *p;
}

__attribute__((noinline, noipa))
void swmmu_test_volatile_store(volatile uint64_t *p)
{
	*p = 1;
	*p = 2;
}

__attribute__((noinline, noipa))
uint64_t swmmu_test_call(const uint64_t *p)
{
	uint64_t before = *p;

	swmmu_test_barrier();
	return before + *p;
}

__attribute__((noinline, noipa))
uint64_t swmmu_test_alias(uint64_t *p, uint64_t *q)
{
	*p = 3;
	*q = 7;
	return *p;
}

__attribute__((noinline, noipa, optimize("O0")))
uint64_t swmmu_test_o0(const uint64_t *p)
{
	return *p;
}
