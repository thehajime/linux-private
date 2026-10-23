#include <linux/nommu_swmmu.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/syscalls.h>
#include <linux/mman.h>
#include <linux/preempt.h>
#include <linux/sched/mm.h>

#include "internal.h"

#define SWMMU_VA_BASE ((uintptr_t)0x1000000000ULL)

struct swmmu_pte {
	struct page *page;
	unsigned int flags;
};

struct swmmu_pagetable {
	refcount_t refs;
	unsigned long nr_ptes;
	struct swmmu_pte *ptes;
	/* Keep the original NOMMU pages reserved while aliases use their VAs. */
	struct vm_region *guard_region;
};

struct swmmu_pagetable_range {
	struct swmmu_pagetable *pagetable;
	unsigned long first;
	unsigned long nr_ptes;
	unsigned int prot;

	unsigned long host_start;
	bool host_mapped;
	/* Some pages may have been removed by overlapping range teardown. */
	bool host_dirty;
};

enum swmmu_pagetable_tx_kind {
	SWMMU_TX_RESIZE,
	SWMMU_TX_SPLIT,
	SWMMU_TX_DROP_RANGE,
	SWMMU_TX_MOVE,
};

enum swmmu_pagetable_replace_kind {
	SWMMU_REPLACE_EXACT,
	SWMMU_REPLACE_HEAD,
	SWMMU_REPLACE_TAIL,
	SWMMU_REPLACE_MIDDLE,
};


struct swmmu_pagetable_tx {
	enum swmmu_pagetable_tx_kind kind;

	struct vm_area_struct *new_vma;

	struct swmmu_pagetable_range *source;
	struct swmmu_pagetable_range *new_range;
	unsigned int replacement_prot;
	unsigned long split_nr_ptes;
	struct swmmu_pagetable_range *drop_range;
	unsigned long drop_first;
	unsigned long drop_nr_ptes;
	struct swmmu_pte *drop_saved;
	bool move_reuses_pagetable;
};


struct nommu_swmmu_clone_item {
	struct list_head node;
	struct vm_area_struct *src_vma;
	struct vm_area_struct *dst_vma;
	struct swmmu_pagetable_range *new_data;
	bool published;
};

struct nommu_swmmu_native_fork_item {
	struct list_head node;
	struct vm_area_struct *src_vma;
	struct vm_area_struct *parent_vma;
	struct vm_area_struct *child_vma;
	struct vma_iterator parent_vmi;
	struct vma_iterator child_vmi;
	struct swmmu_pagetable_range *parent_data;
	struct swmmu_pagetable_range *child_data;
	bool parent_alias_mapped;
	bool child_published;
	unsigned long alias_pages_mapped;
};

struct nommu_swmmu_native_shared_item {
	struct list_head node;
	struct vm_area_struct *src_vma;
	struct vm_area_struct *dst_vma;
	struct vma_iterator child_vmi;
	bool inserted;
	bool published;
};

struct nommu_swmmu_space {
	struct rw_semaphore lock;
	struct mm_struct *mm;
	refcount_t users;
	const struct nommu_swmmu_mem_ops *ops;
	const struct nommu_swmmu_host_ops *host_ops;

	u64 host_generation;
	bool has_native_guard;
};

static struct mm_struct *swmmu_host_active_mm;
static u64 swmmu_host_active_generation;

static inline void *default_kzalloc(size_t size, gfp_t gfp)
{
	return kzalloc(size, gfp);
}

static inline void default_kfree(void *ptr)
{
	kfree(ptr);
}

static inline struct page *default_alloc_page(gfp_t gfp)
{
	return alloc_page(gfp);
}

static inline void default_free_page(struct page *page)
{
	__free_page(page);
}

static inline int default_copy_page(struct page *dst,
				const struct page *src)
{
	copy_highpage(dst, (struct page *)src);
	return 0;
}

static const struct nommu_swmmu_mem_ops default_ops = {
	.zalloc = default_kzalloc,
	.dealloc = default_kfree,
	.page_alloc = default_alloc_page,
	.page_free = default_free_page,
	.page_copy = default_copy_page,
};

static size_t page_align(size_t size)
{
	return (size + SWMMU_PAGE_SIZE - 1) &
	       ~(SWMMU_PAGE_SIZE - 1);
}

/* helper functions for maple tree */
static struct swmmu_pagetable *swmmu_pagetable_alloc(struct nommu_swmmu_space *space,
						unsigned long length)
{
	struct swmmu_pagetable *pt;
	size_t i;
	size_t allocated = 0;

	length = PAGE_ALIGN(length);
	if (!length)
		return NULL;

	pt = space->ops->zalloc(sizeof(*pt), GFP_KERNEL);
	if (!pt)
		return NULL;

	refcount_set(&pt->refs, 1);
	pt->nr_ptes = length / SWMMU_PAGE_SIZE;

	pt->ptes = space->ops->zalloc(
		pt->nr_ptes * sizeof(*pt->ptes),
		GFP_KERNEL);
	if (!pt->ptes) {
		space->ops->dealloc(pt);
		return NULL;
	}

	for (i = 0; i < pt->nr_ptes; i++) {
		pt->ptes[i].page = space->ops->page_alloc(GFP_KERNEL);
		if (!pt->ptes[i].page)
			goto fail_pages;

		clear_highpage(pt->ptes[i].page);
		allocated++;
	}

	return pt;

fail_pages:
	while (allocated)
		space->ops->page_free(pt->ptes[--allocated].page);

	space->ops->dealloc(pt->ptes);
	space->ops->dealloc(pt);
	return NULL;
}

/* XXX: correct only for whole-pagetable ownership */
static void swmmu_pagetable_put(struct nommu_swmmu_space *space,
				struct swmmu_pagetable *pt)
{
	size_t i;

	if (!space || !pt)
		return;

	if (!refcount_dec_and_test(&pt->refs))
		return;

	for (i = 0; i < pt->nr_ptes; i++) {
		if (pt->ptes[i].page)
			space->ops->page_free(pt->ptes[i].page);
	}
	if (pt->guard_region) {
		struct vm_region *region = pt->guard_region;

		if (nommu_swmmu_region_exclusive(region)) {
			unsigned long addr;

			/* Restore the direct map before these pages reach the allocator. */
			for (addr = region->vm_start; addr < region->vm_top;
			     addr += PAGE_SIZE) {
				int ret = space->host_ops->map_page(addr,
					virt_to_page((void *)addr),
					PROT_READ | PROT_WRITE | PROT_EXEC);

				if (ret)
					panic("NOMMU SWMMU: failed to restore direct map at %lx: %d\n",
					      addr, ret);
			}
		}
		nommu_swmmu_put_region(pt->guard_region);
	}

	space->ops->dealloc(pt->ptes);
	space->ops->dealloc(pt);
}

static void swmmu_pagetable_get(struct swmmu_pagetable *pt)
{
	refcount_inc(&pt->refs);
}

static void swmmu_pagetable_inherit_guard(struct swmmu_pagetable *dst,
					 const struct swmmu_pagetable *src)
{
	if (!src->guard_region)
		return;

	nommu_swmmu_get_region(src->guard_region);
	dst->guard_region = src->guard_region;
}

static void swmmu_pagetable_drop_entries(struct nommu_swmmu_space *space,
					struct swmmu_pagetable *pt,
					unsigned long first,
					unsigned long nr_ptes)
{
	unsigned long i;

	lockdep_assert_held_write(&space->lock);

	for (i = 0; i < nr_ptes; i++) {
		struct swmmu_pte *pte = &pt->ptes[first + i];

		if (pte->page) {
			space->ops->page_free(pte->page);
			pte->page = NULL;
		}

		pte->flags = 0;
	}
}

static void swmmu_host_unmap_pages(const struct nommu_swmmu_host_ops *ops,
				   unsigned long start, unsigned long nr_pages)
{
	unsigned long i;

	if (!nr_pages)
		return;
	if (ops->unmap_range) {
		ops->unmap_range(start, nr_pages * SWMMU_PAGE_SIZE);
		return;
	}
	for (i = 0; i < nr_pages; i++)
		ops->unmap_page(start + i * SWMMU_PAGE_SIZE);
}

static int swmmu_host_map_range(struct nommu_swmmu_space *space,
				struct swmmu_pagetable_range *range,
				unsigned long start,
				unsigned int prot,
				bool force_alias)
{
	const struct nommu_swmmu_host_ops *host_ops;
	unsigned long i;
	int ret;

	host_ops = space->host_ops;
	if (!host_ops || !host_ops->map_page ||
	    !host_ops->unmap_page)
		return -EOPNOTSUPP;

	/* Keep overlapping SWMMU VMAs helper-backed instead of aliasing them. */
	if (host_ops->alias_range_allowed &&
	    !host_ops->alias_range_allowed(start,
					   range->nr_ptes * SWMMU_PAGE_SIZE)) {
		range->host_start = start;
		range->host_mapped = false;
		range->prot = prot;
		return 0;
	}

	/*
	 * VMA changes can prepare an inactive mm. Do not install its
	 * fixed-address host aliases over those of the active mm.
	 * nommu_swmmu_activate_mm() will map it when it becomes active.
	 */
	if (!force_alias &&
	    READ_ONCE(swmmu_host_active_mm) != space->mm) {
		range->host_start = start;
		range->host_mapped = false;
		range->prot = prot;
		return 0;
	}

	for (i = 0; i < range->nr_ptes;) {
		struct page *page;
		unsigned long address, nr_pages = 1;

		address = start + i * SWMMU_PAGE_SIZE;
		page = range->pagetable->ptes[range->first + i].page;
		if (!page) {
			ret = -EFAULT;
			goto rollback;
		}

		if (host_ops->map_pages) {
			/* Never merge across a VMA, permission or physical-page gap. */
			while (nr_pages < range->nr_ptes - i) {
				unsigned long index = range->first + i + nr_pages;
				struct page *next = range->pagetable->ptes[index].page;

				if (!next || page_to_pfn(next) !=
						page_to_pfn(page) + nr_pages)
					break;
				nr_pages++;
			}
			ret = host_ops->map_pages(address, page, nr_pages, prot);
		} else {
			ret = host_ops->map_page(address, page, prot);
		}
		if (ret)
			goto rollback;
		i += nr_pages;
	}

	range->host_start = start;
	range->host_mapped = true;
	range->host_dirty = false;
	range->prot = prot;
	return 0;

rollback:
	/* A repair may have started with a partially installed old alias. */
	swmmu_host_unmap_pages(host_ops, start, range->nr_ptes);

	range->host_start = 0;
	range->host_mapped = false;
	range->host_dirty = false;
	return ret;
}

static void swmmu_host_unmap_range(struct nommu_swmmu_space *space,
				struct swmmu_pagetable_range *range)
{
	const struct nommu_swmmu_host_ops *host_ops;
	struct mm_struct *active_mm;

	host_ops = space->host_ops;
	if (!range->host_mapped ||
	    !host_ops || !host_ops->unmap_page)
		return;

	/*
	 * An inactive mm may retain mapping state after its active-mm cache
	 * was invalidated. Its teardown must not remove another mm's aliases.
	 * With no active mm, keep unmapping aliases left by exit or exec.
	 */
	active_mm = READ_ONCE(swmmu_host_active_mm);
	if (active_mm && active_mm != space->mm)
		goto clear_mapping_state;

	swmmu_host_unmap_pages(host_ops, range->host_start, range->nr_ptes);

	/* A detached old range may overlap freshly published replacement VMAs. */
	if (space->mm) {
		unsigned long start = range->host_start;
		unsigned long end = start + range->nr_ptes * SWMMU_PAGE_SIZE;
		VMA_ITERATOR(vmi, space->mm, start);
		struct vm_area_struct *vma;

		for_each_vma(vmi, vma) {
			struct swmmu_pagetable_range *overlap = vma->vm_swmmu_pt_range;

			if (vma->vm_start >= end)
				break;
			if (overlap && overlap->host_mapped)
				overlap->host_dirty = true;
		}
	}

clear_mapping_state:
	range->host_start = 0;
	range->host_mapped = false;
	range->host_dirty = false;
}

void nommu_swmmu_host_alias_invalidate(struct mm_struct *mm)
{
	struct nommu_swmmu_space *space;

	if (!mm)
		return;

	space = mm->swmmu_space;
	if (space)
		WRITE_ONCE(space->host_generation,
			   READ_ONCE(space->host_generation) + 1);

	/*
	 * Retain the actual owner while its generation is dirty. Range teardown
	 * clears the affected aliases; same-mm activation only repairs those
	 * ranges. Clearing the owner here used to force a whole-mm remap after
	 * every munmap, including allocator and loader cleanup.
	 */
}

/* Host callbacks return zero or a negative errno, leaving this internal retry. */
enum { SWMMU_HOST_OWNER_CHANGED = 1 };

static int swmmu_host_map_mm(struct mm_struct *mm, bool repair_only)
{
	struct nommu_swmmu_space *space = mm->swmmu_space;
	VMA_ITERATOR(vmi, mm, 0);
	VMA_ITERATOR(rollback_vmi, mm, 0);
	struct vm_area_struct *vma;
	int ret = 0;

	if (!space || !space->host_ops)
		return -EOPNOTSUPP;

	mmap_read_lock(mm);
	down_write(&space->lock);
	/* Lock acquisition may have scheduled another alias owner. */
	preempt_disable();
	if (READ_ONCE(swmmu_host_active_mm) != (repair_only ? mm : NULL) ||
	    (READ_ONCE(mm->swmmu_mode) != NOMMU_SWMMU_ON &&
	     !READ_ONCE(space->has_native_guard))) {
		ret = SWMMU_HOST_OWNER_CHANGED;
		goto out_unlock;
	}

	if (!repair_only) {
		/* Clear aliases prepared during exec without a published owner. */
		for_each_vma(vmi, vma) {
			if (vma->vm_swmmu_pt_range)
				swmmu_host_unmap_range(space, vma->vm_swmmu_pt_range);
		}
		vma_iter_init(&vmi, mm, 0);
	}

	for_each_vma(vmi, vma) {
		struct swmmu_pagetable_range *range = vma->vm_swmmu_pt_range;

		if (!range)
			continue;
		if (repair_only && range->host_mapped && !range->host_dirty &&
		    range->host_start == vma->vm_start)
			continue;

		ret = swmmu_host_map_range(space, range, vma->vm_start,
					range->prot, true);
		if (ret)
			goto rollback;
	}

	WRITE_ONCE(swmmu_host_active_mm, mm);
	WRITE_ONCE(swmmu_host_active_generation, READ_ONCE(space->host_generation));
	goto out_unlock;

rollback:
	for_each_vma(rollback_vmi, vma) {
		if (vma->vm_swmmu_pt_range)
			swmmu_host_unmap_range(space, vma->vm_swmmu_pt_range);
	}
	WRITE_ONCE(swmmu_host_active_mm, NULL);
	WRITE_ONCE(swmmu_host_active_generation, 0);

out_unlock:
	up_write(&space->lock);
	mmap_read_unlock(mm);
	preempt_enable();
	/*
	 * preempt_enable() may switch to another task, which can install a
	 * different mm's aliases before this task resumes.  Recheck after it
	 * returns so activate_mm() retries instead of returning with stale
	 * aliases for the current task.
	 */
	if (!ret &&
	    (READ_ONCE(swmmu_host_active_mm) != mm ||
	     READ_ONCE(swmmu_host_active_generation) !=
					READ_ONCE(space->host_generation)))
		ret = SWMMU_HOST_OWNER_CHANGED;
	return ret;
}

static int swmmu_host_unmap_mm(struct mm_struct *mm)
{
	struct nommu_swmmu_space *space = mm->swmmu_space;
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;
	int ret = 0;

	mmap_read_lock(mm);
	if (space)
		down_write(&space->lock);
	preempt_disable();
	if (READ_ONCE(swmmu_host_active_mm) != mm) {
		ret = SWMMU_HOST_OWNER_CHANGED;
		goto out_unlock;
	}

	if (space) {
		for_each_vma(vmi, vma) {
			if (vma->vm_swmmu_pt_range)
				swmmu_host_unmap_range(space, vma->vm_swmmu_pt_range);
		}
	}
	WRITE_ONCE(swmmu_host_active_mm, NULL);
	WRITE_ONCE(swmmu_host_active_generation, 0);

out_unlock:
	if (space)
		up_write(&space->lock);
	mmap_read_unlock(mm);
	preempt_enable();
	return ret;
}

static int swmmu_host_protect_range(struct nommu_swmmu_space *space,
				struct swmmu_pagetable_range *range,
				unsigned int prot)
{
	const struct nommu_swmmu_host_ops *host_ops;
	unsigned long length;

	if (!range->host_mapped)
		return 0;

	host_ops = space->host_ops;
	if (!host_ops || !host_ops->protect_range)
		return -EOPNOTSUPP;

	length = range->nr_ptes * SWMMU_PAGE_SIZE;

	return host_ops->protect_range(
		range->host_start,
		length,
		prot);
}

static int __nommu_swmmu_activate_mm(struct mm_struct *mm)
{
	struct nommu_swmmu_space *space;
	struct mm_struct *old_mm;
	bool enabled, unchanged;
	int ret;

retry:
	/*
	 * The active owner is a raw pointer cleared by space_detach(). Pin its
	 * users before taking any sleeping lock: an exec/exit may otherwise
	 * free both the mm and its space while this task waits for mmap_lock.
	 * The backend is single-CPU; prevent preemption across pointer lookup
	 * and mmget_not_zero(), and revalidate ownership after sleeping locks.
	 */
	preempt_disable();
	old_mm = READ_ONCE(swmmu_host_active_mm);
	if (old_mm && !mmget_not_zero(old_mm)) {
		preempt_enable();
		cond_resched();
		goto retry;
	}
	space = mm ? mm->swmmu_space : NULL;
	enabled = mm && (READ_ONCE(mm->swmmu_mode) == NOMMU_SWMMU_ON ||
			 (space && READ_ONCE(space->has_native_guard)));
	unchanged = enabled && space && old_mm == mm &&
		READ_ONCE(swmmu_host_active_generation) ==
		READ_ONCE(space->host_generation);
	preempt_enable();
	/*
	 * The snapshot above can become stale if preempt_enable() schedules a
	 * task that switches the process-wide host aliases.  Do not take the
	 * unchanged fast path unless ownership still matches after rescheduling.
	 */
	if (unchanged &&
	    (READ_ONCE(swmmu_host_active_mm) != mm ||
	     READ_ONCE(swmmu_host_active_generation) !=
					READ_ONCE(space->host_generation)))
		unchanged = false;

	if (unchanged) {
		mmput(old_mm);
		return 0;
	}
	if (!enabled) {
		ret = old_mm ? swmmu_host_unmap_mm(old_mm) : 0;
		if (old_mm)
			mmput(old_mm);
		if (ret == SWMMU_HOST_OWNER_CHANGED ||
		    (!ret && READ_ONCE(swmmu_host_active_mm)))
			goto retry;
		return ret;
	}
	if (!space) {
		if (old_mm)
			mmput(old_mm);
		return -EINVAL;
	}
	if (old_mm == mm) {
		ret = swmmu_host_map_mm(mm, true);
		mmput(old_mm);
	} else {
		ret = old_mm ? swmmu_host_unmap_mm(old_mm) : 0;
		if (old_mm)
			mmput(old_mm);
		if (!ret)
			ret = swmmu_host_map_mm(mm, false);
	}
	if (ret == SWMMU_HOST_OWNER_CHANGED)
		goto retry;
	return ret;
}

int nommu_swmmu_activate_mm(struct mm_struct *mm)
{
	struct nommu_swmmu_space *space;
	bool enabled;
	bool retry;
	int ret;

	do {
		ret = __nommu_swmmu_activate_mm(mm);

		/*
		 * Close the window between the final owner check and returning from
		 * activation.  If this task scheduled inside the operation, another
		 * task may have replaced the aliases; repeat the complete operation
		 * before allowing this task to resume userspace.
		 */
		preempt_disable();
		space = mm ? mm->swmmu_space : NULL;
		enabled = mm && (READ_ONCE(mm->swmmu_mode) == NOMMU_SWMMU_ON ||
				 (space && READ_ONCE(space->has_native_guard)));
		retry = !ret &&
			((enabled && space &&
			  (READ_ONCE(swmmu_host_active_mm) != mm ||
			   READ_ONCE(swmmu_host_active_generation) !=
					READ_ONCE(space->host_generation))) ||
			 (!enabled && READ_ONCE(swmmu_host_active_mm)));
		if (retry)
			preempt_enable();
	} while (retry);
	preempt_enable();

	return ret;
}

static struct swmmu_pagetable_range *swmmu_pagetable_range_create(
	struct nommu_swmmu_space *space,
	struct swmmu_pagetable *pt)
{
	struct swmmu_pagetable_range *pt_range;

	if (!space || !pt)
		return NULL;

	pt_range = space->ops->zalloc(sizeof(*pt_range), GFP_KERNEL);
	if (!pt_range)
		return NULL;

	swmmu_pagetable_get(pt);

	pt_range->pagetable = pt;
	pt_range->first = 0;
	pt_range->nr_ptes = pt->nr_ptes;
	pt_range->prot = PROT_NONE;

	return pt_range;
}

static void swmmu_pagetable_range_free(struct nommu_swmmu_space *space,
					struct swmmu_pagetable_range *data)
{
	if (!space || !data)
		return;

	space->ops->dealloc(data);
}

static void swmmu_pagetable_range_release_locked(struct nommu_swmmu_space *space,
						struct swmmu_pagetable_range *range)
{
	struct swmmu_pagetable *pt;

	if (!space || !range)
		return;

	lockdep_assert_held_write(&space->lock);

	swmmu_host_unmap_range(space, range);

	pt = range->pagetable;
	if (pt && range->nr_ptes)
		swmmu_pagetable_drop_entries(
			space, pt, range->first, range->nr_ptes);

	range->first = 0;
	range->nr_ptes = 0;
	range->pagetable = NULL;

	if (pt)
		swmmu_pagetable_put(space, pt);

	swmmu_pagetable_range_free(space, range);
}

static void swmmu_pagetable_range_release(struct nommu_swmmu_space *space,
					struct swmmu_pagetable_range *range)
{
	if (!space || !range)
		return;

	down_write(&space->lock);
	swmmu_pagetable_range_release_locked(space, range);
	up_write(&space->lock);
}

static void swmmu_pagetable_range_put_locked(struct nommu_swmmu_space *space,
					struct swmmu_pagetable_range *range)
{
	struct swmmu_pagetable *pt;

	if (!space || !range)
		return;

	lockdep_assert_held_write(&space->lock);

	swmmu_host_unmap_range(space, range);
	pt = range->pagetable;

	range->pagetable = NULL;
	range->first = 0;
	range->nr_ptes = 0;

	if (pt)
		swmmu_pagetable_put(space, pt);

	swmmu_pagetable_range_free(space, range);
}

static void swmmu_pagetable_range_put(
	struct nommu_swmmu_space *space,
	struct swmmu_pagetable_range *range)
{
	if (!space || !range)
		return;

	down_write(&space->lock);
	swmmu_pagetable_range_put_locked(space, range);
	up_write(&space->lock);
}

static struct swmmu_pagetable_range *swmmu_pagetable_range_clone(
	struct nommu_swmmu_space *dst_space,
	const struct swmmu_pagetable_range *src)
{
	struct swmmu_pagetable_range *dst;
	size_t i;
	int ret;

	if (!dst_space || !src || !src->pagetable ||
		src->nr_ptes > src->pagetable->nr_ptes ||
		src->nr_ptes >
		src->pagetable->nr_ptes - src->first)
		return NULL;

	lockdep_assert_held_write(&dst_space->lock);

	dst = dst_space->ops->zalloc(sizeof(*dst), GFP_KERNEL);
	if (!dst)
		return NULL;

	dst->pagetable = swmmu_pagetable_alloc(dst_space,
					src->nr_ptes * SWMMU_PAGE_SIZE);
	if (!dst->pagetable) {
		dst_space->ops->dealloc(dst);
		return NULL;
	}

	dst->first = 0;
	dst->nr_ptes = src->nr_ptes;
	dst->prot = src->prot;

	for (i = 0; i < src->nr_ptes; i++) {
		ret = dst_space->ops->page_copy(
			dst->pagetable->ptes[i].page,
			src->pagetable->ptes[src->first + i].page);
		if (ret) {
			swmmu_pagetable_range_release_locked(dst_space, dst);
			return NULL;
		}
	}
	swmmu_pagetable_inherit_guard(dst->pagetable, src->pagetable);

	return dst;
}

static void nommu_swmmu_clear_child_vmas(struct mm_struct *mm)
{
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;

	for_each_vma(vmi, vma)
		vma->vm_swmmu_pt_range = NULL;
}

void nommu_swmmu_vma_close(struct mm_struct *mm,
			   struct vm_area_struct *vma)
{
	struct nommu_swmmu_space *space;
	struct swmmu_pagetable_range *range;

	if (!mm || !vma)
		return;

	range = vma->vm_swmmu_pt_range;
	if (!range)
		return;

	space = mm->swmmu_space;
	/*
	 * Removing a mapped VMA drops its host aliases below. Invalidate the
	 * active-mm cache first so the next activation remaps the surviving
	 * ranges instead of taking the same-mm fast path with a missing alias.
	 */
	if (range->host_mapped)
		nommu_swmmu_host_alias_invalidate(mm);
	vma->vm_swmmu_pt_range = NULL;

	swmmu_pagetable_range_release(space, range);
}

static struct swmmu_pagetable_range *swmmu_pagetable_range_share(
	struct nommu_swmmu_space *space,
	const struct swmmu_pagetable_range *source,
	unsigned long first,
	unsigned long nr_ptes)
{
	struct swmmu_pagetable_range *range;

	if (!space || !source || !source->pagetable || !nr_ptes)
		return NULL;

	if (first < source->first ||
	    nr_ptes > source->nr_ptes -
		       (first - source->first))
		return NULL;

	range = space->ops->zalloc(sizeof(*range), GFP_KERNEL);
	if (!range)
		return NULL;

	swmmu_pagetable_get(source->pagetable);

	range->pagetable = source->pagetable;
	range->first = first;
	range->nr_ptes = nr_ptes;
	range->prot = source->prot;

	return range;
}

static void nommu_swmmu_clone_item_abort(struct mm_struct *dst,
					struct nommu_swmmu_space *dst_space,
					struct nommu_swmmu_clone_item *item)
{
	struct vm_area_struct *removed;
	VMA_ITERATOR(vmi, dst, 0);

	if (!item->published) {
		swmmu_pagetable_range_release_locked(dst_space, item->new_data);
		item->new_data = NULL;
		vm_area_free(item->dst_vma);
		return;
	}

	vma_iter_set(&vmi, item->dst_vma->vm_start);
	removed = mas_erase(&vmi.mas);

	if (WARN_ON_ONCE(removed != item->dst_vma))
		return;

	nommu_swmmu_vma_close(dst, item->dst_vma);
	dst->map_count--;
	vm_area_free(item->dst_vma);
}

/* Prepare one child VMA and its SWMMU metadata without publishing either. */
static int nommu_swmmu_clone_item_prepare(
		struct mm_struct *dst,
		struct nommu_swmmu_space *dst_space,
		struct vm_area_struct *src_vma,
		struct nommu_swmmu_clone_item **result)
{
	struct swmmu_pagetable_range *src_data = src_vma->vm_swmmu_pt_range;
	struct nommu_swmmu_clone_item *item;

	if (!src_data || !src_data->pagetable ||
	    src_data->first > src_data->pagetable->nr_ptes ||
	    src_data->nr_ptes >
	    src_data->pagetable->nr_ptes - src_data->first)
		return -EINVAL;

	item = dst_space->ops->zalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;

	item->src_vma = src_vma;
	item->dst_vma = vm_area_dup(src_vma);
	if (!item->dst_vma) {
		dst_space->ops->dealloc(item);
		return -ENOMEM;
	}

	item->dst_vma->vm_mm = dst;
	item->dst_vma->vm_swmmu_pt_range = NULL;
	if (item->dst_vma->vm_region) {
		vm_area_free(item->dst_vma);
		dst_space->ops->dealloc(item);
		return -EOPNOTSUPP;
	}

	item->new_data = swmmu_pagetable_range_clone(dst_space, src_data);
	if (!item->new_data) {
		vm_area_free(item->dst_vma);
		dst_space->ops->dealloc(item);
		return -ENOMEM;
	}

	*result = item;
	return 0;
}

static unsigned int nommu_swmmu_vma_prot(const struct vm_area_struct *vma)
{
	unsigned int prot = 0;

	if (vma->vm_flags & VM_READ)
		prot |= PROT_READ;
	if (vma->vm_flags & VM_WRITE)
		prot |= PROT_WRITE;
	if (vma->vm_flags & VM_EXEC)
		prot |= PROT_EXEC;

	return prot;
}

/*
 * Check whether this SWMMU VMA has a complete host alias for the requested
 * access.  The caller must hold mmap_lock.  This describes an alias in the
 * UML host process; it does not authorize access through an inactive mm.
 */
static bool swmmu_vma_alias_accessible(const struct vm_area_struct *vma,
				      unsigned long address, size_t size,
				      unsigned int access)
{
	const struct swmmu_pagetable_range *range =
		vma->vm_swmmu_pt_range;
	unsigned long length;

	if (!range || !range->host_mapped || range->host_dirty ||
	    !range->pagetable || !size ||
	    address < vma->vm_start || address > vma->vm_end ||
	    address > ULONG_MAX - size ||
	    size > vma->vm_end - address ||
	    range->host_start != vma->vm_start)
		return false;

	length = vma->vm_end - vma->vm_start;
	if (!PAGE_ALIGNED(length) ||
	    range->nr_ptes != length / SWMMU_PAGE_SIZE ||
	    range->first > range->pagetable->nr_ptes ||
	    range->nr_ptes > range->pagetable->nr_ptes - range->first)
		return false;

	/* SWMMU permissions live in the per-VMA range metadata. */
	if ((range->prot & access) != access)
		return false;

	return true;
}

static bool swmmu_mm_alias_active(struct mm_struct *mm)
{
	struct nommu_swmmu_space *space = mm->swmmu_space;

	return space && READ_ONCE(swmmu_host_active_mm) == mm &&
		READ_ONCE(swmmu_host_active_generation) ==
		READ_ONCE(space->host_generation);
}

/*
 * Recover a transient host-alias miss before treating it as a guest fault.
 * The global alias window can be replaced while a task is descheduled; a
 * valid SWMMU VMA then faults even though its guest backing page still exists.
 * Return 1 when aliases were repaired, 0 when this is an ordinary fault, or
 * a negative errno if the repair itself failed.
 */
int nommu_swmmu_recover_alias_fault(struct mm_struct *mm,
				   unsigned long address,
				   unsigned int access)
{
	struct swmmu_pagetable_range *range;
	struct vm_area_struct *vma;
	unsigned long page_index;
	bool recover = false;
	int ret;

	if (!mm || !mm->swmmu_space)
		return 0;

	mmap_read_lock(mm);
	vma = find_vma(mm, address);
	if (!vma || address < vma->vm_start || address >= vma->vm_end)
		goto out_unlock;

	range = vma->vm_swmmu_pt_range;
	if (!range || !range->pagetable ||
	    (range->prot & access) != access ||
	    address - vma->vm_start >= range->nr_ptes * SWMMU_PAGE_SIZE)
		goto out_unlock;

	page_index = range->first +
		(address - vma->vm_start) / SWMMU_PAGE_SIZE;
	if (page_index >= range->pagetable->nr_ptes ||
	    !range->pagetable->ptes[page_index].page)
		goto out_unlock;

	recover = !swmmu_vma_alias_accessible(vma, address, 1, access) ||
		  !swmmu_mm_alias_active(mm);

out_unlock:
	mmap_read_unlock(mm);
	if (!recover)
		return 0;

	ret = nommu_swmmu_activate_mm(mm);
	return ret ? ret : 1;
}

static bool nommu_swmmu_native_fork_candidate(
		const struct vm_area_struct *vma)
{
	return vma->vm_region && !vma->vm_swmmu_pt_range && !vma->vm_file &&
		!vma->vm_ops && !(vma->vm_flags & VM_SHARED) &&
		(vma->vm_region->vm_flags & VM_MAPPED_COPY) &&
		!vma_test(vma, VMA_DONTCOPY_BIT);
}

static bool swmmu_native_shared_candidate(const struct vm_area_struct *vma)
{
	return vma->vm_region && !vma->vm_swmmu_pt_range &&
		(vma->vm_flags & VM_SHARED) &&
		!vma_test(vma, VMA_DONTCOPY_BIT);
}

static void swmmu_native_shared_abort(struct nommu_swmmu_native_shared_item *item,
				      struct nommu_swmmu_space *space)
{
	struct vm_area_struct *vma = item->dst_vma;

	if (item->inserted) {
		VMA_ITERATOR(vmi, vma->vm_mm, vma->vm_start);
		struct vm_area_struct *removed;

		removed = mas_erase(&vmi.mas);
		if (WARN_ON_ONCE(removed != vma))
			return;

		vma->vm_mm->map_count--;
	}
	if (item->published) {
		if (vma->vm_file) {
			struct address_space *mapping = vma->vm_file->f_mapping;

			i_mmap_lock_write(mapping);
			flush_dcache_mmap_lock(mapping);
			mapping_rmap_tree_remove(vma, mapping);
			flush_dcache_mmap_unlock(mapping);
			i_mmap_unlock_write(mapping);
		}
		vma_close(vma);
		if (vma->vm_file)
			fput(vma->vm_file);
		nommu_swmmu_put_region(vma->vm_region);
	}

	if (item->child_vmi.mas.tree)
		vma_iter_free(&item->child_vmi);
	vm_area_free(vma);
	space->ops->dealloc(item);
}

static int swmmu_native_shared_prepare(struct mm_struct *dst,
				       struct nommu_swmmu_space *space,
				       struct vm_area_struct *src_vma,
				       struct nommu_swmmu_native_shared_item **result)
{
	struct nommu_swmmu_native_shared_item *item;

	item = space->ops->zalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;

	item->src_vma = src_vma;
	item->dst_vma = vm_area_dup(src_vma);
	if (!item->dst_vma) {
		space->ops->dealloc(item);
		return -ENOMEM;
	}

	item->dst_vma->vm_mm = dst;
	vma_clear_flags_mask(item->dst_vma, VMA_LOCKED_MASK);
	*result = item;
	return 0;
}

static void swmmu_native_shared_publish(struct nommu_swmmu_native_shared_item *item)
{
	struct vm_area_struct *vma = item->dst_vma;

	nommu_swmmu_get_region(vma->vm_region);

	if (vma->vm_ops && vma->vm_ops->open)
		vma->vm_ops->open(vma);

	if (vma->vm_file) {
		struct address_space *mapping = vma->vm_file->f_mapping;

		get_file(vma->vm_file);
		i_mmap_lock_write(mapping);
		flush_dcache_mmap_lock(mapping);
		mapping_rmap_tree_insert_after(vma, item->src_vma, mapping);
		flush_dcache_mmap_unlock(mapping);
		i_mmap_unlock_write(mapping);
	}
	item->published = true;
}

static void nommu_swmmu_native_fork_item_abort(
		struct nommu_swmmu_native_fork_item *item,
		struct nommu_swmmu_space *src_space,
		struct nommu_swmmu_space *dst_space)
{
	if (item->child_published) {
		VMA_ITERATOR(vmi, item->child_vma->vm_mm,
			     item->child_vma->vm_start);

		if (WARN_ON_ONCE(mas_erase(&vmi.mas) != item->child_vma))
			return;
		item->child_vma->vm_mm->map_count--;
		item->child_vma->vm_swmmu_pt_range = NULL;
	}
	if (item->parent_vmi.mas.tree)
		vma_iter_free(&item->parent_vmi);
	if (item->child_vmi.mas.tree)
		vma_iter_free(&item->child_vmi);

	if (item->parent_data)
		swmmu_pagetable_range_release_locked(src_space,
						     item->parent_data);
	if (item->child_data)
		swmmu_pagetable_range_release_locked(dst_space,
						     item->child_data);
	if (item->parent_vma) {
		if (item->parent_vma->vm_file)
			fput(item->parent_vma->vm_file);
		vm_area_free(item->parent_vma);
	}
	if (item->child_vma) {
		if (item->child_vma->vm_file)
			fput(item->child_vma->vm_file);
		vm_area_free(item->child_vma);
	}
	src_space->ops->dealloc(item);
}

static int nommu_swmmu_native_fork_item_prepare(
		struct mm_struct *dst, struct mm_struct *src,
		struct nommu_swmmu_space *dst_space,
		struct nommu_swmmu_space *src_space,
		struct vm_area_struct *src_vma,
		struct nommu_swmmu_native_fork_item **result)
{
	struct nommu_swmmu_native_fork_item *item;
	struct swmmu_pagetable *pt;
	unsigned long address;
	unsigned long length;
	unsigned long i;
	int ret;

	length = src_vma->vm_end - src_vma->vm_start;
	if (!length || !PAGE_ALIGNED(src_vma->vm_start) ||
	    !PAGE_ALIGNED(length) ||
	    src_vma->vm_region->vm_start != src_vma->vm_start ||
	    src_vma->vm_region->vm_top != src_vma->vm_end ||
	    !nommu_swmmu_region_exclusive(src_vma->vm_region))
		return -EINVAL;

	item = src_space->ops->zalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;

	item->src_vma = src_vma;
	item->parent_vma = vm_area_dup(src_vma);
	item->child_vma = vm_area_dup(src_vma);
	if (!item->parent_vma || !item->child_vma) {
		ret = -ENOMEM;
		goto error;
	}

	item->parent_vma->vm_mm = src;
	item->child_vma->vm_mm = dst;
	item->parent_vma->vm_region = NULL;
	item->child_vma->vm_region = NULL;
	item->parent_vma->vm_swmmu_pt_range = NULL;
	item->child_vma->vm_swmmu_pt_range = NULL;
	if (item->parent_vma->vm_file)
		get_file(item->parent_vma->vm_file);
	if (item->child_vma->vm_file)
		get_file(item->child_vma->vm_file);

	pt = swmmu_pagetable_alloc(src_space, length);
	if (!pt) {
		ret = -ENOMEM;
		goto error;
	}

	for (i = 0; i < pt->nr_ptes; i++) {
		struct page *src_page;
		struct page *dst_page;

		address = src_vma->vm_start + i * SWMMU_PAGE_SIZE;
		src_page = virt_to_page((void *)address);
		dst_page = pt->ptes[i].page;
		ret = src_space->ops->page_copy(dst_page, src_page);
		if (ret) {
			swmmu_pagetable_put(src_space, pt);
			goto error;
		}
	}

	item->parent_data = swmmu_pagetable_range_create(src_space, pt);
	swmmu_pagetable_put(src_space, pt);
	if (!item->parent_data) {
		ret = -ENOMEM;
		goto error;
	}
	item->parent_data->nr_ptes = length / SWMMU_PAGE_SIZE;
	item->parent_data->prot = nommu_swmmu_vma_prot(src_vma);
	item->parent_vma->vm_swmmu_pt_range = item->parent_data;

	if (vma_test(src_vma, VMA_WIPEONFORK_BIT)) {
		pt = swmmu_pagetable_alloc(dst_space, length);
		if (!pt) {
			ret = -ENOMEM;
			goto error;
		}
		item->child_data = swmmu_pagetable_range_create(dst_space, pt);
		swmmu_pagetable_put(dst_space, pt);
		if (!item->child_data) {
			ret = -ENOMEM;
			goto error;
		}
		item->child_data->nr_ptes = length / SWMMU_PAGE_SIZE;
		item->child_data->prot = nommu_swmmu_vma_prot(src_vma);
	} else {
		item->child_data = swmmu_pagetable_range_clone(
			dst_space, item->parent_data);
		if (!item->child_data) {
			ret = -ENOMEM;
			goto error;
		}
	}
	item->child_vma->vm_swmmu_pt_range = item->child_data;

	vma_iter_init(&item->parent_vmi, src, src_vma->vm_start);
	vma_iter_config(&item->parent_vmi, src_vma->vm_start,
			src_vma->vm_end);
	ret = vma_iter_prealloc(&item->parent_vmi, item->parent_vma);
	if (ret)
		goto error;

	*result = item;
	return 0;

error:
	nommu_swmmu_native_fork_item_abort(item, src_space, dst_space);
	return ret;
}

static int nommu_swmmu_native_fork_map_parent(
		struct nommu_swmmu_native_fork_item *item,
		struct nommu_swmmu_space *space)
{
	const struct nommu_swmmu_host_ops *host_ops = space->host_ops;
	struct swmmu_pagetable_range *range = item->parent_data;
	unsigned long i;
	int ret;

	if (!host_ops || !host_ops->map_page)
		return -EOPNOTSUPP;

	if (host_ops->alias_range_allowed &&
	    !host_ops->alias_range_allowed(item->src_vma->vm_start,
					   range->nr_ptes * SWMMU_PAGE_SIZE)) {
		range->host_start = item->src_vma->vm_start;
		range->host_mapped = false;
		item->parent_alias_mapped = false;
		return 0;
	}

	for (i = 0; i < range->nr_ptes; i++) {
		ret = host_ops->map_page(
			item->src_vma->vm_start + i * SWMMU_PAGE_SIZE,
			range->pagetable->ptes[range->first + i].page,
			range->prot);
		if (ret)
			return ret;
		item->alias_pages_mapped++;
	}

	range->host_start = item->src_vma->vm_start;
	range->host_mapped = true;
	item->parent_alias_mapped = true;
	return 0;
}

static int nommu_swmmu_native_fork_restore_parent(
		struct nommu_swmmu_native_fork_item *item,
		struct nommu_swmmu_space *space)
{
	const struct nommu_swmmu_host_ops *host_ops = space->host_ops;
	unsigned int prot = nommu_swmmu_vma_prot(item->src_vma);
	unsigned long i;
	int ret;

	if (!item->alias_pages_mapped)
		return 0;

	for (i = 0; i < item->alias_pages_mapped; i++) {
		unsigned long address = item->src_vma->vm_start +
					i * SWMMU_PAGE_SIZE;

		ret = host_ops->map_page(address,
					virt_to_page((void *)address), prot);
		if (ret)
			return ret;
	}

	item->parent_data->host_mapped = false;
	item->parent_data->host_start = 0;
	item->parent_alias_mapped = false;
	item->alias_pages_mapped = 0;
	return 0;
}

static int swmmu_find_free_range(struct mm_struct *mm,
				unsigned long min,
				size_t length,
				unsigned long *start)
{
	VMA_ITERATOR(vmi, mm, 0);
	int ret;

	mmap_assert_write_locked(mm);

	if (!length || length > ULONG_MAX - min)
		return -EINVAL;

	if (min >= TASK_SIZE ||
		length > TASK_SIZE - min)
		return -ENOMEM;

	ret = vma_iter_area_lowest(&vmi, min, TASK_SIZE, length);
	if (ret)
		return ret;

	*start = vma_iter_addr(&vmi);
	return 0;
}

const __weak struct nommu_swmmu_host_ops *nommu_swmmu_arch_host_ops(void)
{
	return NULL;
}

/*
 * Create a space using @mem_ops.
 * The operation table must remain alive for the lifetime of the space.
 */
static struct nommu_swmmu_space *
__nommu_swmmu_space_create(
	const struct nommu_swmmu_mem_ops *mem_ops)
{
	struct nommu_swmmu_space *space;

	if (!mem_ops)
		return NULL;

	space = mem_ops->zalloc(sizeof(*space), GFP_KERNEL);
	if (!space)
		return NULL;

	space->ops = mem_ops;
	space->host_ops = nommu_swmmu_arch_host_ops();
	refcount_set(&space->users, 1);
	init_rwsem(&space->lock);
	space->mm = NULL;

	return space;
}

/* for future extensions */
const __weak struct nommu_swmmu_mem_ops *nommu_swmmu_arch_mem_ops(void)
{
	return NULL;
}

struct nommu_swmmu_space *nommu_swmmu_space_create(void)
{
	const struct nommu_swmmu_mem_ops *mem_ops;

	mem_ops = nommu_swmmu_arch_mem_ops();
	if (!mem_ops)
		mem_ops = &default_ops;

	return __nommu_swmmu_space_create(mem_ops);
}

#if IS_ENABLED(CONFIG_NOMMU_SWMMU_KUNIT_TEST)
struct nommu_swmmu_space *
nommu_swmmu_space_create_with_ops(
	const struct nommu_swmmu_mem_ops *mem_ops)
{
	return __nommu_swmmu_space_create(mem_ops);
}
#endif

static void swmmu_space_destroy_final(struct nommu_swmmu_space *space)
{
	const struct nommu_swmmu_mem_ops *mem_ops;

	if (!space)
		return;

	mem_ops = space->ops;
	WARN_ON_ONCE(space->mm);

	mem_ops->dealloc(space);
}

int nommu_swmmu_space_attach(struct mm_struct *mm,
			struct nommu_swmmu_space *space)
{
	if (!mm || !space)
		return -EINVAL;

	if (mm->swmmu_space || space->mm)
		return -EBUSY;

	mm->swmmu_space = space;
	space->mm = mm;

	return 0;
}

void nommu_swmmu_mm_users_zero(struct mm_struct *mm)
{
	int ret;

	if (!mm || READ_ONCE(swmmu_host_active_mm) != mm)
		return;

	/*
	 * mm_users reaching zero means nobody can execute this address space
	 * again. Drop its host aliases now, before __mmput() tears down the
	 * VMAs. space_detach() runs later at mm_count zero, which can be delayed
	 * by active_mm references and is too late to release the alias owner.
	 */
	ret = swmmu_host_unmap_mm(mm);
	WARN_ON_ONCE(ret && ret != SWMMU_HOST_OWNER_CHANGED);
}

/*
 * Detach and release the mm_struct-owned reference to its SWMMU space.
 */
void nommu_swmmu_space_detach(struct mm_struct *mm)
{
	struct nommu_swmmu_space *space;

	if (!mm)
		return;

	space = mm->swmmu_space;
	if (!space)
		return;
	if (READ_ONCE(swmmu_host_active_mm) == mm) {
		WRITE_ONCE(swmmu_host_active_mm, NULL);
		WRITE_ONCE(swmmu_host_active_generation, 0);
	}

	if (space->mm == mm)
		space->mm = NULL;

	mm->swmmu_space = NULL;

	nommu_swmmu_space_put(space);
}

void nommu_swmmu_space_get(struct nommu_swmmu_space *space)
{
	if (!space)
		BUG();

	refcount_inc(&space->users);
}

void nommu_swmmu_space_put(struct nommu_swmmu_space *space)
{
	if (!space)
		return;

	if (!refcount_dec_and_test(&space->users))
		return;

	swmmu_space_destroy_final(space);
}

static bool
nommu_swmmu_space_empty(struct nommu_swmmu_space *space)
{
	struct mm_struct *mm = space->mm;
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;

	mmap_assert_locked(mm);

	for_each_vma(vmi, vma) {
		if (vma->vm_swmmu_pt_range &&
		    !vma->vm_swmmu_pt_range->pagetable->guard_region)
			return false;
	}

	return true;
}

#if IS_ENABLED(CONFIG_NOMMU_SWMMU_KUNIT_TEST)
static struct nommu_swmmu_space *kunit_test_space;
#endif

struct nommu_swmmu_space *nommu_swmmu_current(void)
{
#if IS_ENABLED(CONFIG_NOMMU_SWMMU_KUNIT_TEST)
	/*
	 * KUnit tests may run from a kernel thread without an mm_struct.
	 * This override exists only to exercise compiler-generated calls
	 * against an explicitly selected test space.
	 */
	if (unlikely(kunit_test_space))
		return kunit_test_space;
#endif

	if (!current->mm ||
		READ_ONCE(current->mm->swmmu_mode) != NOMMU_SWMMU_ON)
		return NULL;

	return current->mm->swmmu_space;
}

#if IS_ENABLED(CONFIG_NOMMU_SWMMU_KUNIT_TEST)
void nommu_swmmu_kunit_set_space(struct nommu_swmmu_space *space)
{
	kunit_test_space = space;
}

void nommu_swmmu_kunit_clear_space(void)
{
	kunit_test_space = NULL;
}

int nommu_swmmu_kunit_pagetable_alloc(
	struct nommu_swmmu_space *space,
	size_t size,
	struct swmmu_pagetable **out)
{
	if (!space || !out || !size)
		return -EINVAL;

	*out = swmmu_pagetable_alloc(space, size);
	if (!*out)
		return -ENOMEM;

	return 0;
}

void nommu_swmmu_kunit_pagetable_release(
	struct nommu_swmmu_space *space,
	struct swmmu_pagetable *pt)
{
	swmmu_pagetable_put(space, pt);
}

int nommu_swmmu_kunit_pagetable_range_clone(
	struct nommu_swmmu_space *space,
	const struct swmmu_pagetable_range *src,
	struct swmmu_pagetable_range **out)
{
	if (!space || !src || !out)
		return -EINVAL;

	down_write(&space->lock);
	*out = swmmu_pagetable_range_clone(space, src);
	up_write(&space->lock);
	if (!*out)
		return -ENOMEM;

	return 0;
}

void nommu_swmmu_kunit_pagetable_range_release(
	struct nommu_swmmu_space *space,
	struct swmmu_pagetable_range *data)
{
	swmmu_pagetable_range_release(space, data);
}

#endif


#ifdef CONFIG_DEBUG_VM_MAPLE_TREE
#define SWMMU_VALIDATE_FAIL(fmt, ...)				\
	do {							\
		pr_err("SWMMU validation failed: " fmt,		\
		       ##__VA_ARGS__);				\
		return -EINVAL;					\
	} while (0)

/*
 * Validate the committed SWMMU/VMA state.
 *
 * The caller must hold mmap_lock and space->lock.
 */
static int __nommu_swmmu_validate(struct mm_struct *mm)
{
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;

	mmap_assert_locked(mm);

	for_each_vma(vmi, vma) {
		struct swmmu_pagetable_range *data;

		if (!vma->vm_swmmu_pt_range)
			continue;

		data = vma->vm_swmmu_pt_range;

		VM_BUG_ON_MM(!data->pagetable, mm);
		VM_BUG_ON_MM(!data->nr_ptes, mm);
		VM_BUG_ON_MM(
			vma->vm_end - vma->vm_start !=
			data->nr_ptes * SWMMU_PAGE_SIZE,
			mm);
	}

	return 0;
}

/*
 * Validate the committed SWMMU/VMA state.
 *
 * The caller must hold mmap_lock. This function acquires
 * the SWMMU space read lock.
 */
void nommu_swmmu_validate(struct mm_struct *mm)
{
	struct nommu_swmmu_space *space;
	int ret;

	if (!mm)
		return;

	space = mm->swmmu_space;
	if (!space)
		return;

	mmap_assert_locked(mm);

	/*
	 * mmap_lock is held by the caller. Acquire the SWMMU
	 * space lock after it.
	 */
	down_read(&space->lock);
	ret = __nommu_swmmu_validate(mm);
	up_read(&space->lock);

	VM_BUG_ON_MM(ret, mm);
}

#else
static inline int __nommu_swmmu_validate(struct mm_struct *mm)
{
	return 0;
}

#endif


static void *__swmmu_translate_mm(struct mm_struct *mm,
				unsigned long address,
				size_t size,
				int *status,
				int write)
{
	struct vm_area_struct *vma;
	struct swmmu_pagetable_range *swmmu_vma;
	struct swmmu_pagetable *pt;
	size_t vma_offset;
	size_t page_index;
	size_t page_offset;

	mmap_assert_locked(mm);

	if (!size || address > ULONG_MAX - (size - 1)) {
		*status = -EFAULT;
		return NULL;
	}

	vma = find_vma(mm, address);
	if (!vma ||
		!vma->vm_swmmu_pt_range ||
		address < vma->vm_start ||
		size > vma->vm_end - address) {
		*status = -EFAULT;
		return NULL;
	}

	swmmu_vma = vma->vm_swmmu_pt_range;
	pt = swmmu_vma->pagetable;

	if (write) {
		if (!(swmmu_vma->prot & PROT_WRITE)) {
			*status = -EACCES;
			return NULL;
		}
	} else if (!(swmmu_vma->prot & PROT_READ)) {
		*status = -EACCES;
		return NULL;
	}

	vma_offset = address - vma->vm_start;
	page_index = swmmu_vma->first +
		vma_offset / SWMMU_PAGE_SIZE;
	page_offset = vma_offset % SWMMU_PAGE_SIZE;

	if (page_index >= pt->nr_ptes ||
		page_offset + size > SWMMU_PAGE_SIZE) {
		*status = -EINVAL;
		return NULL;
	}

	return page_address(pt->ptes[page_index].page) + page_offset;
}

int nommu_swmmu_patch_text(struct mm_struct *mm, unsigned long address,
			 const void *expected, const void *replacement, size_t size)
{
	struct swmmu_pagetable_range *range;
	struct nommu_swmmu_space *space;
	struct vm_area_struct *vma;
	unsigned long index, offset;
	void *destination;
	int ret = -EOPNOTSUPP;

	/* Concurrent instruction fetch would require an architecture text poke. */
	if (IS_ENABLED(CONFIG_SMP))
		return -EOPNOTSUPP;

	/* No partially updated instruction, including across a page boundary. */
	if (!mm || !size || size > SWMMU_PAGE_SIZE ||
	    offset_in_page(address) > SWMMU_PAGE_SIZE - size)
		return -EINVAL;

	mmap_write_lock(mm);
	space = mm->swmmu_space;
	if (!space)
		goto unlock_mm;
	down_write(&space->lock);
	vma = find_vma(mm, address);
	if (!vma || address < vma->vm_start || size > vma->vm_end - address ||
	    (vma->vm_flags & VM_SHARED) || !vma->vm_swmmu_pt_range)
		goto unlock_space;
	range = vma->vm_swmmu_pt_range;
	/* Native-fork guards may cover the direct map itself. Leave them alone. */
	if (!(range->prot & PROT_EXEC) || range->pagetable->guard_region)
		goto unlock_space;
	offset = address - vma->vm_start;
	index = range->first + offset / SWMMU_PAGE_SIZE;
	if (index >= range->pagetable->nr_ptes)
		goto unlock_space;
	destination = page_address(range->pagetable->ptes[index].page) +
		      offset % SWMMU_PAGE_SIZE;

	/*
	 * The current SAS backend is uniprocessor. Do not allow guest execution
	 * between the comparison and replacement. mmap_lock also excludes fork,
	 * unmap and protection changes. Private fork copies these backing pages;
	 * no file cache, shared mapping or RX host alias is written here.
	 */
	preempt_disable();
	if (memcmp(destination, expected, size)) {
		ret = -EAGAIN;
	} else {
		memcpy(destination, replacement, size);
		ret = 0;
	}
	preempt_enable();
unlock_space:
	up_write(&space->lock);
unlock_mm:
	mmap_write_unlock(mm);
	return ret;
}

static int swmmu_copy_from_mm(struct mm_struct *mm,
			      unsigned long address,
			      void *destination,
			      size_t size)
{
	int ret = 0;

	mmap_read_lock(mm);

	while (size) {
		size_t offset = address & (SWMMU_PAGE_SIZE - 1);
		size_t chunk = SWMMU_PAGE_SIZE - offset;
		const void *source;

		if (chunk > size)
			chunk = size;

		source = __swmmu_translate_mm(mm, address, chunk,
					      &ret, false);
		if (!source)
			break;

		memcpy(destination, source, chunk);

		address += chunk;
		destination = (unsigned char *)destination + chunk;
		size -= chunk;
	}

	mmap_read_unlock(mm);
	return ret;
}

static int swmmu_copy_to_mm(struct mm_struct *mm,
			uintptr_t address, const void *source,
			size_t size)
{
	int ret = 0;

	mmap_read_lock(mm);

	while (size) {
		size_t page_offset = address & (SWMMU_PAGE_SIZE - 1);
		size_t chunk = SWMMU_PAGE_SIZE - page_offset;
		void *destination;

		if (chunk > size)
			chunk = size;

		destination = __swmmu_translate_mm(mm, address, chunk, &ret, 1);
		if (!destination)
			goto out;

		memcpy(destination, source, chunk);

		address += chunk;
		source = (const unsigned char *)source + chunk;
		size -= chunk;
	}

out:
	mmap_read_unlock(mm);
	return ret;
}

static int __swmmu_check_access_mm(struct mm_struct *, unsigned long, size_t, int);
static int __swmmu_check_dynamic_access_mm(struct mm_struct *,
					   unsigned long, size_t, int);

static int swmmu_dynamic_copy_from_mm(struct mm_struct *mm,
				unsigned long address,
				void *destination,
				size_t size)
{
	struct nommu_swmmu_space *space;
	bool alias_locked = false;
	bool single_page;
	int ret = 0;

	if (!mm || !size)
		return -EINVAL;

	if (unlikely(!address || address < SWMMU_PAGE_SIZE))
		return -EFAULT;
	if (address > ULONG_MAX - size)
		return -EOVERFLOW;

	single_page = size <= SWMMU_PAGE_SIZE -
		(address & (SWMMU_PAGE_SIZE - 1));

	mmap_read_lock(mm);
	space = mm->swmmu_space;
	if (space && READ_ONCE(swmmu_host_active_mm) == mm) {
		down_read(&space->lock);
		alias_locked = swmmu_mm_alias_active(mm);
	} else {
		space = NULL;
	}

	/* A single-page access can be checked while it is copied. */
	if (!single_page) {
		ret = __swmmu_check_dynamic_access_mm(mm, address, size,
						      false);
		if (ret)
			goto out_unlock;
	}

	while (size) {
		struct vm_area_struct *vma;
		unsigned long offset;
		size_t chunk;
		const void *source;

		offset = address & (SWMMU_PAGE_SIZE - 1);
		chunk = SWMMU_PAGE_SIZE - offset;
		if (chunk > size)
			chunk = size;

		vma = find_vma(mm, address);
		if (single_page && (!vma || address < vma->vm_start ||
		    chunk > vma->vm_end - address)) {
			ret = -EFAULT;
			break;
		}
		if (single_page && !vma->vm_swmmu_pt_range &&
		    READ_ONCE(mm->swmmu_mode) == NOMMU_SWMMU_ON &&
		    !(vma->vm_flags & VM_READ)) {
			ret = -EACCES;
			break;
		}
		if (vma && vma->vm_swmmu_pt_range && alias_locked &&
		    swmmu_vma_alias_accessible(vma, address, chunk, PROT_READ)) {
			source = (const void *)address;
			memcpy(destination, source, chunk);
		} else if (vma && vma->vm_swmmu_pt_range) {
			source = __swmmu_translate_mm(mm, address, chunk,
						      &ret, false);
			if (!source)
				break;

			memcpy(destination, source, chunk);
		} else {
			memcpy(destination, (const void __user *)address, chunk);
		}

		address += chunk;
		destination = (unsigned char *)destination + chunk;
		size -= chunk;
	}

out_unlock:
	if (space)
		up_read(&space->lock);
	mmap_read_unlock(mm);
	return ret;
}

static int swmmu_dynamic_copy_to_mm(struct mm_struct *mm,
				    unsigned long address,
				    const void *source,
				    size_t size)
{
	struct nommu_swmmu_space *space;
	bool alias_locked = false;
	bool single_page;
	int ret = 0;

	if (!mm || !size)
		return -EINVAL;
	if (address > ULONG_MAX - size)
		return -EOVERFLOW;

	single_page = size <= SWMMU_PAGE_SIZE -
		(address & (SWMMU_PAGE_SIZE - 1));

	mmap_read_lock(mm);
	space = mm->swmmu_space;
	if (space && READ_ONCE(swmmu_host_active_mm) == mm) {
		down_read(&space->lock);
		alias_locked = swmmu_mm_alias_active(mm);
	} else {
		space = NULL;
	}

	if (!single_page) {
		ret = __swmmu_check_dynamic_access_mm(mm, address, size,
						      true);
		if (ret)
			goto out_unlock;
	}

	while (size) {
		struct vm_area_struct *vma;
		unsigned long offset;
		size_t chunk;
		void *destination;

		offset = address & (SWMMU_PAGE_SIZE - 1);
		chunk = SWMMU_PAGE_SIZE - offset;
		if (chunk > size)
			chunk = size;

		vma = find_vma(mm, address);
		if (!vma || address < vma->vm_start ||
			chunk > vma->vm_end - address) {
			/* XXX: tentative log */
			pr_info_ratelimited("SWMMU missing-vma: pid=%d mm=%px "
					    "address=%lx\n",
					    cpu_tasks[0]->pid, mm, address);
			ret = -EFAULT;
			break;
		}
		if (single_page && !vma->vm_swmmu_pt_range &&
		    READ_ONCE(mm->swmmu_mode) == NOMMU_SWMMU_ON &&
		    !(vma->vm_flags & VM_WRITE)) {
			ret = -EACCES;
			break;
		}
		if (vma->vm_swmmu_pt_range && alias_locked &&
		    swmmu_vma_alias_accessible(vma, address, chunk, PROT_WRITE)) {
			destination = (void *)address;
			memcpy(destination, source, chunk);
		} else if (vma->vm_swmmu_pt_range) {
			destination = __swmmu_translate_mm(mm, address, chunk,
							  &ret, true);
			if (!destination)
				break;

			memcpy(destination, source, chunk);
		} else {
			memcpy((void __user *)address, source, chunk);
		}

		address += chunk;
		source = (const unsigned char *)source + chunk;
		size -= chunk;
	}

out_unlock:
	if (space)
		up_read(&space->lock);
	mmap_read_unlock(mm);
	return ret;
}

static int __swmmu_check_access_mm(struct mm_struct *mm,
				   unsigned long address,
				   size_t size,
				   int write)
{
	int ret = 0;
	unsigned long end;

	mmap_assert_locked(mm);

	if (!size)
		return -EINVAL;

	if (address > ULONG_MAX - size)
		return -EOVERFLOW;

	end = address + size;

	while (address < end) {
		size_t offset = address & (SWMMU_PAGE_SIZE - 1);
		size_t chunk = SWMMU_PAGE_SIZE - offset;

		if (chunk > end - address)
			chunk = end - address;

		if (!__swmmu_translate_mm(mm, address, chunk,
					  &ret, write))
			return ret ? ret : -EFAULT;

		address += chunk;
	}

	return 0;
}

/*
 * Dynamic accesses can target ordinary NOMMU mappings as well as SWMMU
 * mappings. Validate VMA coverage for both kinds and consult the SWMMU page
 * table only when the VMA is managed by SWMMU.
 */
static int __swmmu_check_dynamic_access_mm(struct mm_struct *mm,
					   unsigned long address,
					   size_t size,
					   int write)
{
	unsigned long end;

	mmap_assert_locked(mm);

	if (!size)
		return -EINVAL;
	if (address > ULONG_MAX - size)
		return -EOVERFLOW;

	end = address + size;
	while (address < end) {
		struct vm_area_struct *vma;
		unsigned long offset;
		size_t chunk;
		int ret = 0;

		vma = find_vma(mm, address);
		if (!vma || address < vma->vm_start)
			return -EFAULT;

		offset = address & (SWMMU_PAGE_SIZE - 1);
		chunk = SWMMU_PAGE_SIZE - offset;
		if (chunk > end - address)
			chunk = end - address;
		if (chunk > vma->vm_end - address)
			return -EFAULT;

		if (vma->vm_swmmu_pt_range) {
			if (!__swmmu_translate_mm(mm, address, chunk, &ret, write))
				return ret ? ret : -EFAULT;
		} else if (READ_ONCE(mm->swmmu_mode) == NOMMU_SWMMU_ON &&
			   (write ? !(vma->vm_flags & VM_WRITE) :
			    !(vma->vm_flags & VM_READ))) {
			return -EACCES;
		}

		address += chunk;
	}

	return 0;
}

#include <linux/sched/signal.h>
#include <linux/signal.h>

static long swmmu_signal_access_error(void __user *address,
				      int ret,
				      unsigned int flags)
{
	int code;

	if (!(flags & NOMMU_SWMMU_ACCESS_SIGNAL))
		return ret;

	switch (ret) {
	case -EFAULT:
		code = SEGV_MAPERR;
		break;
	case -EACCES:
		code = SEGV_ACCERR;
		break;
	default:
		return ret;
	}

	force_sig_fault(SIGSEGV, code, address);

	/*
	 * If a handler returns, retry the access after signal delivery.
	 * The default SIGSEGV action terminates the process.
	 */
	return -ERESTARTNOINTR;
}

int nommu_swmmu_check_access(struct nommu_swmmu_space *space,
			uintptr_t address,
			size_t size,
			int write)
{
	struct mm_struct *mm;
	int ret;

	if (!space || !size)
		return -EINVAL;

	mm = space->mm;
	if (!mm)
		return -ENODEV;

	mmap_read_lock(mm);
	ret = __swmmu_check_access_mm(mm, address, size, write);
	mmap_read_unlock(mm);

	return ret;
}



int
nommu_swmmu_load_u64_checked(const void *address,
			     size_t size,
			     u64 *value)
{
	struct mm_struct *mm = current->mm;

	if (!mm || mm->swmmu_mode != NOMMU_SWMMU_ON)
		return -EOPNOTSUPP;

	if (!value ||
	    (size != 1 && size != 2 && size != 4 && size != 8))
		return -EINVAL;

	return swmmu_copy_from_mm(mm, (unsigned long)address,
				  value, size);

}

int
nommu_swmmu_store_u64_checked(void *address,
			      size_t size,
			      u64 value)
{
	int ret;
	struct mm_struct *mm = current->mm;

	if (!mm || mm->swmmu_mode != NOMMU_SWMMU_ON)
		return -EOPNOTSUPP;

	if (size != 1 && size != 2 && size != 4 && size != 8)
		return -EINVAL;

	ret = swmmu_copy_to_mm(mm,
			(uintptr_t)address,
			&value,
			size);

	return ret;
}

uint64_t nommu_swmmu_load_u64(const void *address, size_t size)
{
	uint64_t value = 0;
	int ret;
	struct mm_struct *mm = current->mm;

	if (size != 1 && size != 2 && size != 4 && size != 8)
		BUG();

	if (!mm || mm->swmmu_mode != NOMMU_SWMMU_ON)
		BUG();

	ret = swmmu_copy_from_mm(mm,
				    (uintptr_t)address,
				    &value,
				    size);
	if (ret)
		BUG();

	return value;
}

long nommu_swmmu_store_u64(void *address, size_t size, uint64_t value)
{
	int ret;
	struct mm_struct *mm = current->mm;

	if (size != 1 && size != 2 && size != 4 && size != 8)
		BUG();

	if (!mm || mm->swmmu_mode != NOMMU_SWMMU_ON)
		BUG();

	ret = swmmu_copy_to_mm(mm,
			(uintptr_t)address,
			&value,
			size);
	if (ret)
		BUG();

	return ret;
}

static int swmmu_dynamic_cmpxchg_u32_mm(struct mm_struct *mm,
					unsigned long address,
					u32 expected,
					u32 desired,
					u32 *observed)
{
	struct vm_area_struct *vma;
	void *destination;
	int ret = 0;

	if (!mm || !observed)
		return -EINVAL;

	if (address & (sizeof(u32) - 1))
		return -EINVAL;

	mmap_read_lock(mm);

	vma = find_vma(mm, address);
	if (!vma || !vma->vm_swmmu_pt_range) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}

	destination = __swmmu_translate_mm(mm, address, sizeof(u32),
					   &ret, true);
	if (!destination)
		goto out_unlock;

	/*
	 * The backing page is kernel-addressable. cmpxchg() preserves the
	 * atomic operation that mallocng's a_cas() requires.
	 */
	*observed = cmpxchg((u32 *)destination, expected, desired);

out_unlock:
	mmap_read_unlock(mm);
	return ret;
}

int nommu_swmmu_copy_to_user(void __user *address, const void *source,
			size_t size)
{
	if (!current->mm)
		return -EOPNOTSUPP;

	return swmmu_dynamic_copy_to_mm(
		current->mm,
		(unsigned long)address,
		source,
		size);
}

int nommu_swmmu_copy_from_user(void *destination, const void __user *address,
			size_t size)
{
	if (!current->mm)
		return -EOPNOTSUPP;

	return swmmu_dynamic_copy_from_mm(
		current->mm,
		(unsigned long)address,
		destination,
		size);
}

bool nommu_swmmu_usercopy_requires_translation(unsigned long address)
{
	struct mm_struct *mm = current->mm;

	return address >= UM_NOMMU_SWMMU_VA_BASE ||
		(mm && READ_ONCE(mm->swmmu_mode) == NOMMU_SWMMU_ON);
}
EXPORT_SYMBOL_GPL(nommu_swmmu_usercopy_requires_translation);

int nommu_swmmu_get_user_pages(unsigned long start, int nr_pages,
			       unsigned int gup_flags, bool pin,
			       struct page **pages)
{
	struct mm_struct *mm = current->mm;
	unsigned long address = start;
	int i, ret = 0;

	if (!mm || !pages || nr_pages <= 0)
		return -EINVAL;
	if (start < SWMMU_VA_BASE)
		return -EOPNOTSUPP;

	mmap_read_lock(mm);
	for (i = 0; i < nr_pages; i++, address += PAGE_SIZE) {
		struct swmmu_pagetable_range *range;
		struct swmmu_pagetable *pt;
		struct vm_area_struct *vma;
		struct page *page;
		unsigned long index;
		unsigned int prot = (gup_flags & FOLL_WRITE) ?
			PROT_WRITE : PROT_READ;

		if (address > ULONG_MAX - PAGE_SIZE) {
			ret = -EFAULT;
			break;
		}

		vma = find_vma(mm, address);
		if (!vma || address < vma->vm_start ||
		    !vma->vm_swmmu_pt_range) {
			ret = i ? -EFAULT : -EOPNOTSUPP;
			break;
		}
		range = vma->vm_swmmu_pt_range;
		if (!(range->prot & prot)) {
			ret = -EFAULT;
			break;
		}
		pt = range->pagetable;
		index = range->first +
			((address - vma->vm_start) >> PAGE_SHIFT);
		if (index >= pt->nr_ptes || !pt->ptes[index].page) {
			ret = -EFAULT;
			break;
		}

		page = pt->ptes[index].page;
		if (pin && (gup_flags & FOLL_LONGTERM) &&
		    !folio_is_longterm_pinnable(page_folio(page))) {
			ret = -EFAULT;
			break;
		}
		ret = try_grab_folio(page_folio(page), 1,
				     (pin ? FOLL_PIN : FOLL_GET) |
				     (gup_flags & FOLL_PCI_P2PDMA));
		if (ret)
			break;
		pages[i] = page;
	}
	if (pin)
		mm_flags_set(MMF_HAS_PINNED, mm);
	mmap_read_unlock(mm);

	if (ret)
		return i ? i : ret;
	return i;
}
EXPORT_SYMBOL_GPL(nommu_swmmu_get_user_pages);

EXPORT_SYMBOL_GPL(nommu_swmmu_copy_to_user);
EXPORT_SYMBOL_GPL(nommu_swmmu_copy_from_user);


#if IS_ENABLED(CONFIG_NOMMU_SWMMU_KUNIT_TEST)
void nommu_swmmu_kunit_set_host_ops(struct nommu_swmmu_space *space,
				 const struct nommu_swmmu_host_ops *ops)
{
	space->host_ops = ops;
}

int nommu_swmmu_kunit_load_mm(struct mm_struct *mm,
			      uintptr_t address,
			      size_t size,
			      u64 *value)
{
	if (!mm || !value)
		return -EINVAL;

	if (size != 1 && size != 2 &&
	    size != 4 && size != 8)
		return -EINVAL;

	return swmmu_copy_from_mm(mm, address, value, size);
}

int nommu_swmmu_kunit_store_mm(struct mm_struct *mm,
			       uintptr_t address,
			       size_t size,
			       u64 value)
{
	if (!mm)
		return -EINVAL;

	if (size != 1 && size != 2 &&
	    size != 4 && size != 8)
		return -EINVAL;

	return swmmu_copy_to_mm(mm, address, &value, size);
}

int nommu_swmmu_kunit_dynamic_copy_to_mm(struct mm_struct *mm,
					 unsigned long address,
					 const void *source, size_t size)
{
	return swmmu_dynamic_copy_to_mm(mm, address, source, size);
}

int nommu_swmmu_kunit_dynamic_copy_from_mm(struct mm_struct *mm,
					   unsigned long address,
					   void *destination, size_t size)
{
	return swmmu_dynamic_copy_from_mm(mm, address, destination, size);
}

bool nommu_swmmu_kunit_vma_alias_accessible(struct mm_struct *mm,
					   unsigned long address,
					   size_t size,
					   unsigned int access)
{
	struct vm_area_struct *vma;
	bool accessible = false;

	if (!mm)
		return false;

	mmap_read_lock(mm);
	vma = find_vma(mm, address);
	if (vma && address >= vma->vm_start)
		accessible = swmmu_vma_alias_accessible(vma, address, size,
							 access);
	mmap_read_unlock(mm);

	return accessible;
}

const void *nommu_swmmu_kunit_range_pagetable(
	const struct swmmu_pagetable_range *range)
{
	return range ? range->pagetable : NULL;
}

unsigned long nommu_swmmu_kunit_range_first(
	const struct swmmu_pagetable_range *range)
{
	return range ? range->first : 0;
}

unsigned long nommu_swmmu_kunit_range_nr_ptes(
	const struct swmmu_pagetable_range *range)
{
	return range ? range->nr_ptes : 0;
}

unsigned int nommu_swmmu_kunit_range_access(
	const struct swmmu_pagetable_range *range)
{
	return range ? range->prot : PROT_NONE;
}

unsigned long nommu_swmmu_kunit_pagetable_nr_ptes(
	const struct swmmu_pagetable *pt)
{
	return pt ? pt->nr_ptes : 0;
}

struct page *nommu_swmmu_kunit_pagetable_page(
	const struct swmmu_pagetable *pagetable,
	unsigned long index)
{
	if (!pagetable || index >= pagetable->nr_ptes)
		return NULL;

	return pagetable->ptes[index].page;
}

int nommu_swmmu_kunit_range_create(
	struct nommu_swmmu_space *space,
	struct swmmu_pagetable *pagetable,
	unsigned long first,
	unsigned long nr_ptes,
	unsigned int prot,
	struct swmmu_pagetable_range **out)
{
	struct swmmu_pagetable_range *pt_range;

	pt_range = swmmu_pagetable_range_create(space, pagetable);
	if (!pt_range)
		return -1;
	pt_range->first = first;
	pt_range->nr_ptes = nr_ptes;
	pt_range->prot = prot;

	*out = pt_range;
	return 0;
}
#endif

int nommu_swmmu_dup_mmap(struct mm_struct *dst,
			 struct mm_struct *src)
{
	struct nommu_swmmu_space *src_space;
	struct nommu_swmmu_space *dst_space;
	struct nommu_swmmu_clone_item *item;
	struct nommu_swmmu_clone_item *tmp;
	struct nommu_swmmu_native_fork_item *native_item;
	struct nommu_swmmu_native_fork_item *native_tmp;
	struct nommu_swmmu_native_shared_item *shared_item;
	struct nommu_swmmu_native_shared_item *shared_tmp;
	struct vm_area_struct *src_vma;
	LIST_HEAD(prepared);
	LIST_HEAD(native_prepared);
	LIST_HEAD(shared_prepared);
	VMA_ITERATOR(src_vmi, src, 0);
	int ret = 0;

	if (!dst || !src)
		return -EINVAL;

	/* dup_mmap() holds both locks when calling us. */
	mmap_assert_write_locked(src);
	mmap_assert_write_locked(dst);

	src_space = src->swmmu_space;
	dst_space = dst->swmmu_space;

	if (!src_space || !dst_space || src_space == dst_space)
		return -EINVAL;

	down_write(&src_space->lock);
	down_write_nested(&dst_space->lock, SINGLE_DEPTH_NESTING);

	nommu_swmmu_clear_child_vmas(dst);

	for_each_vma(src_vmi, src_vma) {
		if (nommu_swmmu_native_fork_candidate(src_vma)) {
			/*
			 * The parent and child must both be switchable to their
			 * private backing at this exact VA.  Publishing SWMMU VMA
			 * metadata while the host alias guard refuses the VA would
			 * leave both mms using the original shared native mapping.
			 */
			if (src_space->host_ops &&
			    src_space->host_ops->alias_range_allowed &&
			    !src_space->host_ops->alias_range_allowed(
					src_vma->vm_start,
					src_vma->vm_end - src_vma->vm_start)) {
				ret = -EOPNOTSUPP;
				goto rollback;
			}

			ret = nommu_swmmu_native_fork_item_prepare(
				dst, src, dst_space, src_space, src_vma,
				&native_item);
			if (ret)
				goto rollback;

			list_add_tail(&native_item->node, &native_prepared);
			continue;
		}

		if (swmmu_native_shared_candidate(src_vma)) {
			ret = swmmu_native_shared_prepare(dst, dst_space, src_vma,
							  &shared_item);
			if (ret)
				goto rollback;

			list_add_tail(&shared_item->node, &shared_prepared);
			continue;
		}

		if (!src_vma->vm_swmmu_pt_range)
			continue;

		ret = nommu_swmmu_clone_item_prepare(dst, dst_space, src_vma,
						     &item);
		if (ret)
			goto rollback;

		list_add_tail(&item->node, &prepared);
	}

	/* Publish prepared SWMMU clones before changing the parent mappings. */
	list_for_each_entry(item, &prepared, node) {
		VMA_ITERATOR(dst_vmi, dst, item->dst_vma->vm_start);

		vma_iter_config(&dst_vmi, item->dst_vma->vm_start,
				item->dst_vma->vm_end);
		ret = vma_iter_prealloc(&dst_vmi, item->dst_vma);
		if (ret)
			goto rollback;

		vma_iter_store_new(&dst_vmi, item->dst_vma);
		dst->map_count++;

		item->dst_vma->vm_swmmu_pt_range = item->new_data;
		if (item->new_data->pagetable->guard_region)
			dst_space->has_native_guard = true;
		item->new_data = NULL;
		item->published = true;
	}

	/* Preallocate against the current child tree for every insertion. */
	list_for_each_entry(native_item, &native_prepared, node) {
		vma_iter_init(&native_item->child_vmi, dst,
			      native_item->child_vma->vm_start);
		vma_iter_config(&native_item->child_vmi,
				native_item->child_vma->vm_start,
				native_item->child_vma->vm_end);
		ret = vma_iter_prealloc(&native_item->child_vmi,
					 native_item->child_vma);
		if (ret)
			goto rollback;
		vma_iter_store_new(&native_item->child_vmi,
					 native_item->child_vma);
		dst->map_count++;
		native_item->child_published = true;
	}

	/* Reserve every Maple Tree node before acquiring shared references. */
	list_for_each_entry(shared_item, &shared_prepared, node) {
		vma_iter_init(&shared_item->child_vmi, dst,
			      shared_item->dst_vma->vm_start);
		vma_iter_config(&shared_item->child_vmi,
				shared_item->dst_vma->vm_start,
				shared_item->dst_vma->vm_end);
		ret = vma_iter_prealloc(&shared_item->child_vmi,
					shared_item->dst_vma);
		if (ret)
			goto rollback;

		/* Preallocate and publish each entry before preparing the next. */
		vma_iter_store_new(&shared_item->child_vmi,
				   shared_item->dst_vma);
		dst->map_count++;
		shared_item->inserted = true;
	}

	/* Map every parent replacement before publishing any VMA changes. */
	list_for_each_entry(native_item, &native_prepared, node) {
		ret = nommu_swmmu_native_fork_map_parent(native_item,
							 src_space);
		if (ret)
			goto rollback_aliases;
	}

	/*
	 * All allocations, Maple Tree nodes, and host aliases are ready.
	 * Nothing below this point may fail.
	 */
	list_for_each_entry(shared_item, &shared_prepared, node)
		swmmu_native_shared_publish(shared_item);

	list_for_each_entry(native_item, &native_prepared, node) {
		struct vm_area_struct *old_vma = native_item->src_vma;

		vma_start_write(old_vma);
		vma_start_write(native_item->parent_vma);
		vma_mark_detached(old_vma);
		vma_iter_store_new(&native_item->parent_vmi,
				   native_item->parent_vma);
		native_item->parent_vma->vm_swmmu_pt_range =
			native_item->parent_data;
		native_item->parent_data = NULL;

		native_item->child_vma->vm_swmmu_pt_range =
			native_item->child_data;
		native_item->child_data = NULL;

		/* Keep the original direct-map addresses out of the allocator. */
		native_item->parent_vma->vm_swmmu_pt_range->pagetable->guard_region =
			old_vma->vm_region;
		nommu_swmmu_get_region(old_vma->vm_region);
		native_item->child_vma->vm_swmmu_pt_range->pagetable->guard_region =
			old_vma->vm_region;

		vma_close(old_vma);
		if (old_vma->vm_file)
			fput(old_vma->vm_file);
		vm_area_free(old_vma);
	}

	if (!list_empty(&native_prepared)) {
		u64 generation = READ_ONCE(src_space->host_generation) + 1;

		WRITE_ONCE(src_space->host_generation, generation);
		WRITE_ONCE(swmmu_host_active_mm, src);
		WRITE_ONCE(swmmu_host_active_generation, generation);
		WRITE_ONCE(src->swmmu_mode, NOMMU_SWMMU_ON);
		WRITE_ONCE(dst->swmmu_mode, NOMMU_SWMMU_ON);
		src_space->has_native_guard = true;
		dst_space->has_native_guard = true;
	}

	list_for_each_entry_safe(item, tmp, &prepared, node) {
		list_del(&item->node);
		dst_space->ops->dealloc(item);
	}
	list_for_each_entry_safe(native_item, native_tmp,
				 &native_prepared, node) {
		list_del(&native_item->node);
		vma_iter_free(&native_item->parent_vmi);
		vma_iter_free(&native_item->child_vmi);
		src_space->ops->dealloc(native_item);
	}
	list_for_each_entry_safe(shared_item, shared_tmp,
				 &shared_prepared, node) {
		list_del(&shared_item->node);
		vma_iter_free(&shared_item->child_vmi);
		dst_space->ops->dealloc(shared_item);
	}

	up_write(&dst_space->lock);
	up_write(&src_space->lock);

	return 0;

rollback_aliases:
	list_for_each_entry_safe_reverse(native_item, native_tmp,
					 &native_prepared, node) {
		int restore_ret;

		restore_ret = nommu_swmmu_native_fork_restore_parent(
			native_item, src_space);
		if (WARN_ON_ONCE(restore_ret))
			panic("NOMMU SWMMU: failed to restore native fork mapping\n");
	}

rollback:
	list_for_each_entry_safe(item, tmp, &prepared, node) {
		list_del(&item->node);

		nommu_swmmu_clone_item_abort(dst, dst_space, item);
		dst_space->ops->dealloc(item);
	}
	list_for_each_entry_safe(native_item, native_tmp,
				 &native_prepared, node) {
		list_del(&native_item->node);
		nommu_swmmu_native_fork_item_abort(native_item,
						   src_space, dst_space);
	}
	list_for_each_entry_safe(shared_item, shared_tmp,
				 &shared_prepared, node) {
		list_del(&shared_item->node);
		swmmu_native_shared_abort(shared_item, dst_space);
	}

	up_write(&dst_space->lock);
	up_write(&src_space->lock);

	return ret;
}

static void swmmu_resize_abort(struct nommu_swmmu_space *space,
			struct swmmu_pagetable_tx *tx)
{
	if (!space || !tx)
		return;

	if (tx->new_range) {
		swmmu_pagetable_range_release(
			space, tx->new_range);
		tx->new_range = NULL;
	}

	tx->source = NULL;
	tx->kind = 0;
}

static void swmmu_resize_commit(struct nommu_swmmu_space *space,
				struct swmmu_pagetable_tx *tx)
{
	struct swmmu_pagetable_range *source;
	struct swmmu_pagetable_range *new_range;
	struct swmmu_pagetable *old_pt;
	struct swmmu_pagetable *new_pt;

	if (!space || !tx || tx->kind != SWMMU_TX_RESIZE ||
	    !tx->source || !tx->new_range)
		return;

	source = tx->source;
	new_range = tx->new_range;
	old_pt = source->pagetable;
	new_pt = new_range->pagetable;

	source->pagetable = new_pt;
	source->first = new_range->first;
	source->nr_ptes = new_range->nr_ptes;
	source->prot = new_range->prot;

	new_range->pagetable = NULL;
	swmmu_pagetable_range_free(space, new_range);

	tx->source = NULL;
	tx->new_range = NULL;
	tx->kind = 0;

	swmmu_pagetable_put(space, old_pt);
}

static int swmmu_pagetable_drop_range_prepare(struct nommu_swmmu_space *space,
					struct swmmu_pagetable_range *range,
					unsigned long first,
					unsigned long nr_ptes,
					struct swmmu_pagetable_tx *tx)
{
	struct swmmu_pagetable *pt;
	size_t bytes;

	if (!range || !range->pagetable || !tx || !nr_ptes)
		return -EINVAL;

	pt = range->pagetable;

	if (first != range->first ||
	    nr_ptes != range->nr_ptes)
		return -EINVAL;

	if (first > pt->nr_ptes ||
	    nr_ptes > pt->nr_ptes - first)
		return -EINVAL;

	if (nr_ptes > SIZE_MAX / sizeof(*tx->drop_saved))
		return -EOVERFLOW;

	bytes = nr_ptes * sizeof(*tx->drop_saved);

	memset(tx, 0, sizeof(*tx));

	tx->drop_saved = space->ops->zalloc(bytes, GFP_KERNEL);
	if (!tx->drop_saved)
		return -ENOMEM;

	memcpy(tx->drop_saved, &pt->ptes[first], bytes);

	tx->kind = SWMMU_TX_DROP_RANGE;
	tx->drop_range = range;
	tx->drop_first = first;
	tx->drop_nr_ptes = nr_ptes;

	return 0;
}

static void swmmu_pagetable_drop_range_commit(struct nommu_swmmu_space *space,
					struct swmmu_pagetable_tx *tx)
{
	struct swmmu_pagetable_range *range;

	if (!space || !tx ||
	    tx->kind != SWMMU_TX_DROP_RANGE ||
	    !tx->drop_range)
		return;

	range = tx->drop_range;

	swmmu_pagetable_drop_entries(
		space, range->pagetable,
		tx->drop_first,  tx->drop_nr_ptes);

	/*
	 * This first implementation requires the dropped range to be
	 * exactly represented by @range.
	 */
	range->first = 0;
	range->nr_ptes = 0;

	if (tx->drop_saved) {
		space->ops->dealloc(tx->drop_saved);
		tx->drop_saved = NULL;
	}
	tx->drop_range = NULL;
	tx->drop_nr_ptes = 0;
	tx->kind = 0;
}

static void swmmu_pagetable_drop_range_abort(struct nommu_swmmu_space *space,
					struct swmmu_pagetable_tx *tx)
{
	if (!tx || tx->kind != SWMMU_TX_DROP_RANGE)
		return;

	if (tx->drop_saved) {
		space->ops->dealloc(tx->drop_saved);
		tx->drop_saved = NULL;
	}

	tx->drop_saved = NULL;
	tx->drop_range = NULL;
	tx->drop_nr_ptes = 0;
	tx->kind = 0;
}

static int swmmu_pagetable_resize_prepare(struct nommu_swmmu_space *space,
					struct swmmu_pagetable_range *source,
					size_t new_size,
					struct swmmu_pagetable_tx *tx)
{
	struct swmmu_pagetable_range *new_range;
	struct swmmu_pagetable *old_pt;
	struct swmmu_pagetable *new_pt;
	size_t old_nr_ptes;
	size_t new_nr_ptes;
	size_t copy_count;
	size_t i;
	int ret;

	if (!space || !source || !source->pagetable ||
	    !tx || !new_size)
		return -EINVAL;

	if (!IS_ALIGNED(new_size, SWMMU_PAGE_SIZE))
		return -EINVAL;

	old_pt = source->pagetable;
	old_nr_ptes = source->nr_ptes;
	new_nr_ptes = new_size / SWMMU_PAGE_SIZE;

	if (!old_nr_ptes || !new_nr_ptes)
		return -EINVAL;

	if (source->first > old_pt->nr_ptes ||
	    old_nr_ptes > old_pt->nr_ptes - source->first)
		return -EINVAL;

	if (new_nr_ptes == old_nr_ptes)
		return -EINVAL;

	memset(tx, 0, sizeof(*tx));

	new_pt = swmmu_pagetable_alloc(
		space, new_nr_ptes * SWMMU_PAGE_SIZE);
	if (!new_pt)
		return -ENOMEM;

	new_range = swmmu_pagetable_range_create(space, new_pt);
	if (!new_range) {
		swmmu_pagetable_put(space, new_pt);
		return -ENOMEM;
	}

	new_range->first = 0;
	new_range->nr_ptes = new_nr_ptes;
	new_range->prot = source->prot;

	copy_count = min(old_nr_ptes, new_nr_ptes);

	for (i = 0; i < copy_count; i++) {
		ret = space->ops->page_copy(
			new_pt->ptes[i].page,
			old_pt->ptes[source->first + i].page);
		if (ret) {
			swmmu_pagetable_range_release_locked(space, new_range);
			return ret;
		}
	}
	swmmu_pagetable_inherit_guard(new_pt, old_pt);

	tx->kind = SWMMU_TX_RESIZE;
	tx->source = source;
	tx->new_range = new_range;

	return 0;
}

static int swmmu_expand_prepare(struct vm_area_struct *vma,
				unsigned long new_end,
				void **state)
{
	struct swmmu_pagetable_tx *tx;
	struct nommu_swmmu_space *space;
	size_t new_size;
	int ret;

	if (!vma || !vma->vm_swmmu_pt_range ||
	    new_end <= vma->vm_start)
		return -EINVAL;

	space = vma->vm_mm->swmmu_space;
	if (!space)
		return -EINVAL;

	new_size = new_end - vma->vm_start;

	down_write(&space->lock);

	tx = kzalloc(sizeof(*tx), GFP_KERNEL);
	if (!tx) {
		up_write(&space->lock);
		return -ENOMEM;
	}

	ret = swmmu_pagetable_resize_prepare(
		space,
		vma->vm_swmmu_pt_range,
		new_size,
		tx);
	if (ret) {
		kfree(tx);
		up_write(&space->lock);
		return ret;
	}

	*state = tx;
	return 0;
}

static void swmmu_expand_commit(struct vm_area_struct *vma,
				void *state)
{
	struct swmmu_pagetable_tx *tx = state;
	struct nommu_swmmu_space *space;
	struct swmmu_pagetable_range *range;
	bool alias_was_mapped;

	if (!tx)
		return;

	space = vma->vm_mm->swmmu_space;
	range = tx->source;
	alias_was_mapped = range->host_mapped;
	if (alias_was_mapped) {
		/*
		 * Resize replaces every backing page. Drop the old host aliases
		 * before committing the new page table, and invalidate the active
		 * mm so it remaps the resized VMA before returning to userspace.
		 */
		swmmu_host_unmap_range(space, range);
		WRITE_ONCE(space->host_generation,
			   READ_ONCE(space->host_generation) + 1);
	}

	swmmu_resize_commit(space, tx);
	kfree(tx);
	up_write(&space->lock);
}

static void swmmu_expand_abort(struct vm_area_struct *vma,
			       void *state)
{
	struct swmmu_pagetable_tx *tx = state;
	struct nommu_swmmu_space *space;

	if (!tx)
		return;

	space = vma->vm_mm->swmmu_space;

	swmmu_resize_abort(space, tx);
	kfree(tx);
	up_write(&space->lock);
}

static int swmmu_split_prepare(struct vm_area_struct *vma,
			struct vm_area_struct *new,
			unsigned long addr,
			bool new_below,
			void **state)
{
	struct nommu_swmmu_space *space = vma->vm_mm->swmmu_space;
	struct swmmu_pagetable_range *old_pt_range;
	struct swmmu_pagetable_range *new_pt_range;
	struct swmmu_pagetable_tx *tx;
	unsigned long split_pages;

	old_pt_range = vma->vm_swmmu_pt_range;
	if (!old_pt_range || !old_pt_range->pagetable)
		return -EINVAL;

	if (addr <= vma->vm_start || addr >= vma->vm_end)
		return -EINVAL;

	if (!IS_ALIGNED(addr - vma->vm_start, SWMMU_PAGE_SIZE))
		return -EINVAL;

	split_pages = (addr - vma->vm_start) / SWMMU_PAGE_SIZE;

	if (!split_pages || split_pages >= old_pt_range->nr_ptes)
		return -EINVAL;

	if (old_pt_range->first >
	    old_pt_range->pagetable->nr_ptes)
		return -EINVAL;

	if (old_pt_range->nr_ptes >
	    old_pt_range->pagetable->nr_ptes - old_pt_range->first)
		return -EINVAL;

	if (new_below) {
		if (new->vm_start != vma->vm_start ||
			new->vm_end != addr)
			return -EINVAL;
	} else {
		if (new->vm_start != addr ||
			new->vm_end != vma->vm_end)
			return -EINVAL;
	}

	tx = space->ops->zalloc(sizeof(*tx), GFP_KERNEL);
	if (!tx)
		return -ENOMEM;

	new_pt_range = space->ops->zalloc(sizeof(*new_pt_range), GFP_KERNEL);
	if (!new_pt_range) {
		space->ops->dealloc(tx);
		return -ENOMEM;
	}

	tx->kind = SWMMU_TX_SPLIT;
	tx->source = old_pt_range;
	tx->new_range = new_pt_range;
	tx->split_nr_ptes = split_pages;

	/*
	 * Both VMAs refer to the same immutable pagetable object.
	 */
	swmmu_pagetable_get(tx->source->pagetable);

	new_pt_range->pagetable = tx->source->pagetable;
	new_pt_range->prot = old_pt_range->prot;

	if (new_below) {
		new_pt_range->first = old_pt_range->first;
		new_pt_range->nr_ptes = split_pages;
	} else {
		new_pt_range->first =
			old_pt_range->first + split_pages;
		new_pt_range->nr_ptes =
			old_pt_range->nr_ptes - split_pages;
	}

	*state = tx;
	return 0;
}

static void swmmu_split_commit(struct vm_area_struct *vma,
			       struct vm_area_struct *new,
			       unsigned long addr,
			       bool new_below,
			       void *state)
{
	struct nommu_swmmu_space *space = vma->vm_mm->swmmu_space;
	struct swmmu_pagetable_tx *tx = state;

	if (new_below) {
		tx->source->first += tx->split_nr_ptes;
		tx->source->nr_ptes -= tx->split_nr_ptes;
	} else {
		tx->source->nr_ptes = tx->split_nr_ptes;
	}

	/*
	 * The host mapping already covers both resulting ranges. It is
	 * only the per-range metadata that needs to be split.
	 */
	if (tx->source->host_mapped) {
		tx->source->host_start = vma->vm_start;
		tx->new_range->host_start = new->vm_start;
		tx->new_range->host_mapped = true;
		tx->new_range->host_dirty = tx->source->host_dirty;
	}

	new->vm_swmmu_pt_range = tx->new_range;
	tx->new_range = NULL;

	space->ops->dealloc(tx);
}

static void swmmu_split_abort(struct vm_area_struct *vma,
			struct vm_area_struct *new,
			void *state)
{
	struct nommu_swmmu_space *space = vma->vm_mm->swmmu_space;
	struct swmmu_pagetable_tx *tx = state;

	if (!tx)
		return;

	if (tx->new_range) {
		swmmu_pagetable_range_put(space, tx->new_range);
		tx->new_range = NULL;
	}
	space->ops->dealloc(tx);
}

static void swmmu_remove_detached_vma(struct mm_struct *mm,
					struct vm_area_struct *vma)
{
	if (!mm || !vma)
		return;

	nommu_swmmu_vma_close(mm, vma);
	vma_close(vma);

	if (vma->vm_file)
		fput(vma->vm_file);

	vm_area_free(vma);
}

static int swmmu_replace_prepare(struct vma_replace_struct *vrs)
{
	struct swmmu_pagetable_tx *tx;
	struct vm_area_struct *insert;
	struct mm_struct *mm;
	struct nommu_swmmu_space *space;
	struct swmmu_pagetable *pt;
	struct swmmu_pagetable_range *data;
	unsigned long length;

	if (!vrs || !vrs->vms || !vrs->insert)
		return -EINVAL;

	tx = vrs->backend_state;
	if (!tx)
		return -EINVAL;

	insert = vrs->insert;
	mm = insert->vm_mm;
	space = mm->swmmu_space;
	length = insert->vm_end - insert->vm_start;

	if (!space || !length)
		return -EINVAL;

	pt = swmmu_pagetable_alloc(space, length);
	if (!pt)
		return -ENOMEM;

	data = swmmu_pagetable_range_create(space, pt);
	if (!data) {
		swmmu_pagetable_put(space, pt);
		return -ENOMEM;
	}

	swmmu_pagetable_put(space, pt);
	data->prot = tx->replacement_prot;
	insert->vm_swmmu_pt_range = data;
	vrs->backend_state = data;

	return 0;
}

static void swmmu_replace_abort(struct vma_replace_struct *vrs)
{
	struct vm_area_struct *insert;
	struct mm_struct *mm;

	if (!vrs || !vrs->insert)
		return;

	insert = vrs->insert;
	mm = insert->vm_mm;

	if (insert->vm_swmmu_pt_range) {
		swmmu_pagetable_range_put(mm->swmmu_space,
				       insert->vm_swmmu_pt_range);
		insert->vm_swmmu_pt_range = NULL;
	}

	vrs->backend_state = NULL;
}

static void swmmu_replace_commit(struct vma_replace_struct *vrs)
{
	if (!vrs)
		return;

	vrs->backend_state = NULL;
}

static struct vm_area_struct *swmmu_fixed_alloc_vma(struct mm_struct *mm,
						struct vm_area_struct *template,
						unsigned long start,
						unsigned long end,
						vma_flags_t vma_flags,
						unsigned long pgoff)
{
	struct vm_area_struct *vma;

	if (template)
		vma = vm_area_dup(template);
	else
		vma = vm_area_alloc(mm);

	if (!vma)
		return NULL;

	vma->vm_mm = mm;
	vma->vm_start = start;
	vma->vm_end = end;
	vma->vm_swmmu_pt_range = NULL;

	vm_flags_init(vma, vma_flags_to_legacy(vma_flags));
	vma_set_pgoff(vma, pgoff);

	return vma;
}

static int swmmu_move_prepare(struct vma_remap_struct *vrm,
			struct vm_area_struct *src,
			struct vm_area_struct **dst,
			bool *dst_linked,
			void **state)
{
	struct swmmu_pagetable_tx *tx;
	struct swmmu_pagetable_range *src_range;
	struct swmmu_pagetable_range *new_range;
	struct swmmu_pagetable *new_pt;
	struct nommu_swmmu_space *space;
	struct vm_area_struct *new_vma;
	unsigned long old_nr;
	unsigned long new_nr;
	unsigned long i;
	int ret;

	if (!vrm || !src || !dst || !dst_linked || !state)
		return -EINVAL;

	src_range = src->vm_swmmu_pt_range;
	if (!src_range || !src_range->pagetable)
		return -EINVAL;

	space = src->vm_mm->swmmu_space;
	if (!space)
		return -EINVAL;

	old_nr = vrm->old_len / SWMMU_PAGE_SIZE;
	new_nr = vrm->new_len / SWMMU_PAGE_SIZE;

	if (!old_nr || !new_nr ||
	    old_nr > src_range->nr_ptes)
		return -EINVAL;

	new_vma = swmmu_fixed_alloc_vma(src->vm_mm, src, vrm->new_addr,
		vrm->new_addr + vrm->new_len, src->flags, src->vm_pgoff);
	if (!new_vma)
		return -ENOMEM;

	new_vma->vm_swmmu_pt_range = NULL;

	down_write(&space->lock);

	tx = space->ops->zalloc(sizeof(*tx), GFP_KERNEL);
	if (!tx) {
		ret = -ENOMEM;
		goto out_unlock_free_vma;
	}

	tx->kind = SWMMU_TX_MOVE;
	tx->source = src_range;
	tx->new_vma = new_vma;

	if (old_nr == new_nr) {
		new_range = swmmu_pagetable_range_share(
			space,
			src_range,
			src_range->first,
			old_nr);
		if (!new_range) {
			ret = -ENOMEM;
			goto out_unlock_free_tx;
		}

		tx->new_range = new_range;
		tx->move_reuses_pagetable = true;
		*dst = new_vma;
		*dst_linked = false;
		*state = tx;
		return 0;
	}

	new_pt = swmmu_pagetable_alloc(
		space, new_nr * SWMMU_PAGE_SIZE);
	if (!new_pt) {
		ret = -ENOMEM;
		goto out_unlock_free_tx;
	}

	new_range = swmmu_pagetable_range_create(space, new_pt);
	swmmu_pagetable_put(space, new_pt);
	if (!new_range) {
		ret = -ENOMEM;
		goto out_unlock_free_tx;
	}

	new_range->first = 0;
	new_range->nr_ptes = new_nr;
	new_range->prot = src_range->prot;

	for (i = 0; i < min(old_nr, new_nr); i++) {
		ret = space->ops->page_copy(
			new_range->pagetable->ptes[i].page,
			src_range->pagetable->ptes[
				src_range->first + i].page);
		if (ret) {
			swmmu_pagetable_range_release_locked(
				space, new_range);
			goto out_unlock_free_tx;
		}
	}
	swmmu_pagetable_inherit_guard(new_range->pagetable,
					 src_range->pagetable);

	tx->new_range = new_range;
	*dst = new_vma;
	*dst_linked = false;
	*state = tx;
	return 0;

out_unlock_free_tx:
	space->ops->dealloc(tx);
out_unlock_free_vma:
	up_write(&space->lock);
	vm_area_free(new_vma);
	return ret;
}

static void swmmu_move_commit(struct vma_remap_struct *vrm,
			      struct vm_area_struct *src,
			      struct vm_area_struct *dst,
			      void *state)
{
	struct swmmu_pagetable_tx *tx = state;
	struct nommu_swmmu_space *space;

	if (!tx || tx->kind != SWMMU_TX_MOVE)
		return;

	space = src->vm_mm->swmmu_space;

	dst->vm_swmmu_pt_range = tx->new_range;
	tx->new_range = NULL;

	if (tx->move_reuses_pagetable) {
		tx->source->first = 0;
		tx->source->nr_ptes = 0;
	}

	tx->source = NULL;
	tx->new_vma = NULL;
	tx->kind = 0;

	space->ops->dealloc(tx);
	up_write(&space->lock);
}

static void swmmu_move_abort(struct vma_remap_struct *vrm,
			     struct vm_area_struct *src,
			     struct vm_area_struct *dst,
			     void *state)
{
	struct swmmu_pagetable_tx *tx = state;
	struct nommu_swmmu_space *space;

	if (!tx || tx->kind != SWMMU_TX_MOVE)
		return;

	space = src->vm_mm->swmmu_space;

	if (tx->new_range) {
		if (tx->move_reuses_pagetable)
			swmmu_pagetable_range_put_locked(space, tx->new_range);
		else
			swmmu_pagetable_range_release_locked(space,
							tx->new_range);

		tx->new_range = NULL;
	}

	tx->source = NULL;
	tx->new_vma = NULL;
	tx->kind = 0;

	space->ops->dealloc(tx);
	up_write(&space->lock);
}

static unsigned long swmmu_get_unmapped_area(struct vma_remap_struct *vrm)
{
	unsigned long addr;
	int ret;

	if (vrm->flags & MREMAP_FIXED)
		return vrm->new_addr;

	ret = swmmu_find_free_range(
		vrm->mm,
		SWMMU_VA_BASE,
		vrm->new_len,
		&addr);
	if (ret)
		return ret;

	return addr;
}

static int swmmu_check_remap(struct vma_remap_struct *vrm)
{
	struct vm_area_struct *vma;

	if (!vrm || !vrm->vma)
		return -EINVAL;

	vma = vrm->vma;

	if (!vma->vm_swmmu_pt_range)
		return -EINVAL;

	/*
	 * Current SWMMU remap supports complete VMA ranges only.
	 */
	if (vma->vm_start != vrm->addr ||
	    vma->vm_end != vrm->addr + vrm->old_len)
		return -EINVAL;

	if ((vrm->flags & MREMAP_DONTUNMAP) ||
	    ((vrm->flags & MREMAP_FIXED) &&
	     vrm->new_len < vrm->old_len))
		return -EOPNOTSUPP;

	return 0;
}

static const struct vma_mapping_ops swmmu_vma_mapping_ops = {
	.split_prepare = swmmu_split_prepare,
	.split_commit = swmmu_split_commit,
	.split_abort = swmmu_split_abort,
	.remove_detached = swmmu_remove_detached_vma,
	.replace_prepare = swmmu_replace_prepare,
	.replace_commit = swmmu_replace_commit,
	.replace_abort = swmmu_replace_abort,
	.expand_prepare = swmmu_expand_prepare,
	.expand_commit = swmmu_expand_commit,
	.expand_abort = swmmu_expand_abort,
	.move_prepare = swmmu_move_prepare,
	.move_commit = swmmu_move_commit,
	.move_abort = swmmu_move_abort,
	.get_unmapped_area = swmmu_get_unmapped_area,
	.check_remap = swmmu_check_remap,
};

const struct vma_mapping_ops *vma_mapping_ops_for_mm(struct mm_struct *mm)
{
	return &swmmu_vma_mapping_ops;
}

static unsigned long swmmu_mmap_fixed_range_replace(struct mm_struct *mm,
						struct vm_area_struct *first_vma,
						struct vm_area_struct *template,
						unsigned long start,
						unsigned long end,
						unsigned long prot,
						vma_flags_t vma_flags,
						unsigned long pgoff);
static unsigned long swmmu_mmap_fixed_replace(struct mm_struct *mm,
					enum swmmu_pagetable_replace_kind kind,
					struct nommu_swmmu_space *space,
					struct vm_area_struct *old_vma,
					unsigned long start,
					unsigned long end,
					unsigned long prot,
					vma_flags_t vma_flags,
					unsigned long pgoff)
{
	if (!mm || !space || !old_vma || start >= end)
		return -EINVAL;

	if (kind != SWMMU_REPLACE_HEAD &&
	    kind != SWMMU_REPLACE_TAIL)
		return -EOPNOTSUPP;

	if (!old_vma->vm_swmmu_pt_range ||
	    old_vma->vm_file ||
	    old_vma->vm_region)
		return -EOPNOTSUPP;

	if (start < old_vma->vm_start ||
	    end > old_vma->vm_end)
		return -EINVAL;

	if (kind == SWMMU_REPLACE_HEAD) {
		if (start != old_vma->vm_start ||
		    end >= old_vma->vm_end)
			return -EINVAL;
	} else {
		if (start <= old_vma->vm_start ||
		    end != old_vma->vm_end)
			return -EINVAL;
	}

	return swmmu_mmap_fixed_range_replace(mm, old_vma, old_vma,
					      start, end, prot,
					      vma_flags, pgoff);
}

static unsigned long swmmu_mmap_fixed_range_replace(struct mm_struct *mm,
						struct vm_area_struct *first_vma,
						struct vm_area_struct *template,
						unsigned long start,
						unsigned long end,
						unsigned long prot,
						vma_flags_t vma_flags,
						unsigned long pgoff)
{
	struct vm_area_struct *insert;
	struct vma_munmap_struct vms;
	struct vma_replace_struct vrs;
	struct maple_tree mt_detach;
	MA_STATE(mas_detach, &mt_detach, 0, 0);
	VMA_ITERATOR(vmi, mm, start);
	struct swmmu_pagetable_tx tx = {};
	int ret;

	if (!mm || !first_vma || start >= end)
		return -EINVAL;

	insert = swmmu_fixed_alloc_vma(mm, template, start, end,
				vma_flags, pgoff);
	if (!insert)
		return -ENOMEM;

	insert->vm_swmmu_pt_range = NULL;
	tx.replacement_prot = prot;

	mt_init_flags(&mt_detach,
		      vmi.mas.tree->ma_flags &
		      MT_FLAGS_LOCK_MASK);
	mt_on_stack(mt_detach);

	vma_init_munmap(&vms, &vmi, first_vma, start, end, NULL, false,
			&swmmu_vma_mapping_ops);

	vma_replace_init(&vrs, &vms, insert,
			 &swmmu_vma_mapping_ops);
	vrs.backend_state = &tx;

	ret = vma_replace_prepare(&vrs, &mas_detach);
	if (ret)
		goto abort;

	vma_replace_commit(&vrs, &mas_detach, mm, swmmu_remove_detached_vma);

	__mt_destroy(&mt_detach);

	validate_mm(mm);
	nommu_swmmu_validate(mm);

	return start;

abort:
	vma_replace_abort(&vrs, &mas_detach);
	__mt_destroy(&mt_detach);
	return ret;
}


static unsigned long
__do_mmap_swmmu(struct mm_struct *mm,
		struct file *file,
		unsigned long addr,
		unsigned long len,
		unsigned long prot,
		unsigned long flags,
		vma_flags_t vma_flags,
		unsigned long pgoff,
		unsigned long *populate,
		struct list_head *uf)
{
	struct nommu_swmmu_space *space;
	struct swmmu_pagetable *pt;
	struct swmmu_pagetable_range *swmmu_vma;
	struct vm_area_struct *vma;
	struct vm_area_struct *replace_vma = NULL;
	VMA_ITERATOR(vmi, mm, 0);
	unsigned long start;
	unsigned long end;
	int ret;
	int overlap_count;

	if (!mm)
		return -EINVAL;

	space = mm->swmmu_space;
	if (!space)
		return -EINVAL;

	mmap_assert_write_locked(mm);

	/* Match the NOMMU backend's per-mm VMA limit. */
	if (mm->map_count >= get_sysctl_max_map_count())
		return -ENOMEM;

	/*
	 * Select start/end using VMA/range management.
	 * This replaces swmmu_find_free_range().
	 */
	/*
	 * currently partially implemented:
	 *
	 * address == 0, no fixed flag:
	 *     allocate at next_address
	 *
	 * nonzero address, no fixed flag:
	 *     reject with -EINVAL
	 *
	 * MAP_FIXED:
	 *     exact address, but reject overlap for now
	 *
	 * MAP_FIXED_NOREPLACE:
	 *     exact address, reject overlap
	 */
	if (flags & MAP_FIXED || flags & MAP_FIXED_NOREPLACE) {
		if (!addr || !PAGE_ALIGNED(addr)) {
			return -EINVAL;
		}

		len = PAGE_ALIGN(len);
		if (!len || addr > ULONG_MAX - len)
			return -EINVAL;
		end = addr + len;

		overlap_count = vma_range_count_overlaps(mm, addr, end, &replace_vma);
		if (overlap_count < 0)
			return overlap_count;

		if (flags & MAP_FIXED_NOREPLACE) {
			if (overlap_count)
				return -EEXIST;
		}

		if (overlap_count == 1) {
			if (!replace_vma->vm_swmmu_pt_range)
				return -EOPNOTSUPP;

			if (replace_vma->vm_start == addr &&
			    replace_vma->vm_end == end)
				return swmmu_mmap_fixed_range_replace(mm, replace_vma,
								replace_vma, addr, end,
								prot, vma_flags, pgoff);
			else if (replace_vma->vm_start == addr &&
				 end < replace_vma->vm_end)
				return swmmu_mmap_fixed_replace(mm, SWMMU_REPLACE_HEAD,
								space, replace_vma, addr,
								end, prot, vma_flags, pgoff);
			else if (addr > replace_vma->vm_start &&
				 replace_vma->vm_end == end)
				return swmmu_mmap_fixed_replace(mm, SWMMU_REPLACE_TAIL,
								space, replace_vma, addr,
								end, prot, vma_flags, pgoff);
			else if (addr > replace_vma->vm_start &&
				 end < replace_vma->vm_end)
				return swmmu_mmap_fixed_range_replace(mm, replace_vma,
								replace_vma, addr, end,
								prot, vma_flags, pgoff);
			else
				return -EINVAL;
		} else if (overlap_count > 1) {
			return swmmu_mmap_fixed_range_replace(mm, replace_vma,
							NULL, addr, end,
							prot, vma_flags, pgoff);
		}
		start = addr;
	}
	else if (addr != 0) {
		ret = swmmu_find_free_range(mm, page_align(addr), len, &start);
		if (ret)
			ret = swmmu_find_free_range(mm, SWMMU_VA_BASE, len, &start);
		if (ret)
			return ret;
	}
	else {
		ret = swmmu_find_free_range(mm, SWMMU_VA_BASE, len, &start);
		if (ret)
			return ret;
	}
	end = start + PAGE_ALIGN(len);

	pt = swmmu_pagetable_alloc(space, end - start);
	if (!pt)
		return -ENOMEM;

	vma = swmmu_fixed_alloc_vma(mm, NULL, start, end,
					vma_flags, pgoff);
	if (!vma) {
		swmmu_pagetable_put(space, pt);
		return -ENOMEM;
	}

	swmmu_vma = swmmu_pagetable_range_create(space, pt);
	if (!swmmu_vma) {
		vm_area_free(vma);
		swmmu_pagetable_put(space, pt);
		return -ENOMEM;
	}

	swmmu_vma->prot = prot;
	vma->vm_swmmu_pt_range = swmmu_vma;

	ret = swmmu_host_map_range(mm->swmmu_space, swmmu_vma, vma->vm_start,
				prot, false);
	if (ret) {
		vma->vm_swmmu_pt_range = NULL;
		swmmu_pagetable_range_release(mm->swmmu_space, swmmu_vma);
		vm_area_free(vma);
		swmmu_pagetable_put(mm->swmmu_space, pt);
		return ret;
	}

	vma_iter_config(&vmi, start, end);
	ret = vma_iter_prealloc(&vmi, vma);
	if (ret)
		goto rollback_new_mapping;

	if (replace_vma) {
		vma_start_write(replace_vma);
		vma_start_write(vma);
	}

	vma_iter_store_new(&vmi, vma);

	/* MAP_FIXED with exact replacement */
	if (replace_vma) {
		vma_mark_detached(replace_vma);
		mm->map_count--;

		nommu_swmmu_vma_close(mm, replace_vma);
		vma_close(replace_vma);

		if (replace_vma->vm_file)
			fput(replace_vma->vm_file);

		vm_area_free(replace_vma);
	}

	mm->map_count++;
	return start;

rollback_new_mapping:
	vma->vm_swmmu_pt_range = NULL;
	swmmu_pagetable_range_release(space, swmmu_vma);
	vm_area_free(vma);
	swmmu_pagetable_put(space, pt);
	return ret;
}

/*
 * currently, file, prot, populate, uf are not used and ignored.
 * will be updated.
 */
static unsigned long do_mmap_swmmu(struct file *file,
			unsigned long addr,
			unsigned long len,
			unsigned long prot,
			unsigned long flags,
			vma_flags_t vma_flags,
			unsigned long pgoff,
			unsigned long *populate,
			struct list_head *uf)
{
	if (!current->mm)
		return -EINVAL;

	return __do_mmap_swmmu(current->mm, file, addr, len,
			       prot, flags, vma_flags,
			       pgoff, populate, uf);
}

#if IS_ENABLED(CONFIG_NOMMU_SWMMU_KUNIT_TEST)
unsigned long nommu_swmmu_kunit_mmap_mm(
	struct mm_struct *mm,
	unsigned long addr,
	unsigned long len,
	unsigned long prot,
	unsigned long flags)
{
	unsigned long populate = 0;
	unsigned long ret;

	if (!mm || !len)
		return -EINVAL;

	mmap_write_lock(mm);

	ret = __do_mmap_swmmu(mm, NULL, addr, len,
			      prot, flags,
			      EMPTY_VMA_FLAGS, 0,
			      &populate, NULL);

	mmap_write_unlock(mm);

	return ret;
}

int nommu_swmmu_kunit_unmap_mm(struct mm_struct *mm,
			       unsigned long address,
			       size_t size)
{
	int ret;

	if (!mm || !size)
		return -EINVAL;

	mmap_write_lock(mm);
	ret = do_munmap(mm, address, size, NULL);
	mmap_write_unlock(mm);

	return ret;
}

int nommu_swmmu_kunit_drop_range(struct nommu_swmmu_space *space,
				struct swmmu_pagetable_range *range,
				unsigned long first,
				unsigned long nr_ptes,
				bool commit)
{
	struct swmmu_pagetable_tx tx = {};
	int ret;

	if (!space || !range)
		return -EINVAL;

	down_write(&space->lock);

	ret = swmmu_pagetable_drop_range_prepare(space, range, first, nr_ptes, &tx);
	if (ret)
		goto out_unlock;

	if (commit)
		swmmu_pagetable_drop_range_commit(space, &tx);
	else
		swmmu_pagetable_drop_range_abort(space, &tx);

out_unlock:
	up_write(&space->lock);
	return ret;
}
#endif

static int
nommu_swmmu_validate_mmap_request(struct file *file,
				  unsigned long addr,
				  unsigned long len,
				  unsigned long prot,
				  unsigned long flags,
				  vma_flags_t vma_flags,
				  unsigned long pgoff,
				  unsigned long *populate,
				  struct list_head *uf)
{
	if (file ||
	    !(flags & MAP_ANONYMOUS) ||
	    !(flags & MAP_PRIVATE))
		return -EOPNOTSUPP;

	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
		return -EINVAL;

	if (prot == PROT_WRITE ||
		prot == PROT_EXEC ||
		prot == (PROT_WRITE | PROT_EXEC))
		return -EOPNOTSUPP;

	return 0;
}

unsigned long do_mmap(struct file *file,
			unsigned long addr,
			unsigned long len,
			unsigned long prot,
			unsigned long flags,
			vma_flags_t vma_flags,
			unsigned long pgoff,
			unsigned long *populate,
			struct list_head *uf)
{
	struct mm_struct *mm = current->mm;
	int ret;

	/* FIXME: temporary policy */
	if (flags & MAP_GROWSDOWN) {
		if (file || !(flags & MAP_ANONYMOUS) ||
			!(flags & MAP_PRIVATE))
			return -EOPNOTSUPP;

		/*
		 * SWMMU does not implement automatic VMA growth yet.  The
		 * initial exec stack is allocated eagerly for the complete
		 * requested range, so MAP_GROWSDOWN is only a classification
		 * hint here.
		 */
		flags &= ~MAP_GROWSDOWN;

		if (nommu_swmmu_get_mode(mm) == NOMMU_SWMMU_ON) {
			ret = nommu_swmmu_validate_mmap_request(
				file, addr, len, prot, flags,
				vma_flags, pgoff, populate, uf);
			if (ret)
				return ret;

			return do_mmap_swmmu(file, addr, len, prot, flags,
					     vma_flags, pgoff, populate, uf);
		}

		/*
		 * Preserve the existing native NOMMU behavior for legacy/off
		 * mode processes.
		 */
		pr_debug_ratelimited(
			"NOMMU SWMMU: keeping MAP_GROWSDOWN mapping native\n");
		return do_mmap_nommu(file, addr, len, prot, flags,
				     vma_flags, pgoff, populate, uf);
	}

	if ((prot & PROT_EXEC) && nommu_swmmu_get_mode(mm) != NOMMU_SWMMU_ON)
		return do_mmap_nommu(file, addr, len, prot, flags,
				vma_flags, pgoff, populate, uf);

	if (nommu_swmmu_get_mode(mm) == NOMMU_SWMMU_ON) {
		ret = nommu_swmmu_validate_mmap_request(
			file, addr, len, prot, flags,
			vma_flags, pgoff, populate, uf);

		if (ret == 0)
			return do_mmap_swmmu(file, addr, len, prot, flags,
					     vma_flags, pgoff,
					     populate, uf);

		/* EOPNOTSUPP fall back to nommu backend */
		if (ret != -EOPNOTSUPP)
			return ret;
	}

	return do_mmap_nommu(file, addr, len, prot, flags,
			     vma_flags, pgoff, populate, uf);
}

int do_munmap(struct mm_struct *mm,
	unsigned long start, size_t len, struct list_head *uf)
{
	VMA_ITERATOR(vmi, mm, start);
	struct vm_area_struct *vma;
	unsigned long end;

	len = PAGE_ALIGN(len);
	if (len == 0)
		return -EINVAL;

	if (len > ULONG_MAX - start)
		return -EINVAL;

	end = start + len;

	vma = vma_find(&vmi, end);
	if (vma && vma->vm_swmmu_pt_range) {
		return do_vmi_munmap(&vmi, mm, start, len, uf, false);
	}

	return do_munmap_nommu(mm, start, len, uf);
}

#if IS_ENABLED(CONFIG_NOMMU_SWMMU_KUNIT_TEST)
unsigned long nommu_swmmu_kunit_mremap_mm(struct mm_struct *mm,
					unsigned long addr,
					unsigned long old_len,
					unsigned long new_len,
					unsigned long flags,
					unsigned long new_addr)
{
	unsigned long ret;
	struct vma_remap_struct vrm = {
		.addr = untagged_addr(addr),
		.old_len = old_len,
		.new_len = new_len,
		.flags = flags,
		.new_addr = new_addr,

		.remap_type = MREMAP_INVALID, /* We set later. */
		.mm = mm,
		.backend = vma_mapping_ops_for_mm(mm),
		.backend_state = NULL,
	};


	if (!mm)
		return -EINVAL;

	ret = do_mremap(&vrm);

	return ret;
}
#endif

/* prctl interface */
int nommu_swmmu_set_mode(struct mm_struct *mm,
			 unsigned long mode)
{
	struct nommu_swmmu_space *space;
	bool empty;

	if (!mm)
		return -EINVAL;

	if (SWMMU_VA_BASE >= TASK_SIZE)
		BUG();

	if (mode != NOMMU_SWMMU_OFF &&
	    mode != NOMMU_SWMMU_ON)
		return -EINVAL;

	mmap_write_lock(mm);

	space = mm->swmmu_space;
	if (!space) {
		mmap_write_unlock(mm);
		return -EINVAL;
	}

	down_read(&space->lock);
	empty = nommu_swmmu_space_empty(space);
	up_read(&space->lock);

	/*
	 * Do not disable while SWMMU mappings still exist.
	 * Otherwise existing pointers would change meaning.
	 */
	if (mode == NOMMU_SWMMU_OFF && !empty) {
		mmap_write_unlock(mm);
		return -EBUSY;
	}

	WRITE_ONCE(mm->swmmu_mode, mode);

	mmap_write_unlock(mm);
	return 0;
}

long nommu_swmmu_get_mode(struct mm_struct *mm)
{
	if (!mm)
		return -EINVAL;

	return READ_ONCE(mm->swmmu_mode);
}

static bool swmmu_valid_access_size(size_t size)
{
	return size == 1 || size == 2 || size == 4 || size == 8;
}

SYSCALL_DEFINE1(nommu_swmmu_alloc, size_t, size)
{
	struct mm_struct *mm = current->mm;

	if (!mm || mm->swmmu_mode != NOMMU_SWMMU_ON)
		return -EOPNOTSUPP;

	return vm_mmap(NULL, 0, size,
		       PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS,
		       0);
}

SYSCALL_DEFINE1(nommu_swmmu_free, void __user *, address)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long start = (unsigned long)address;
	unsigned long length;
	int ret;

	if (!mm || mm->swmmu_mode != NOMMU_SWMMU_ON)
		return -EOPNOTSUPP;

	mmap_write_lock(mm);

	vma = find_vma(mm, start);
	if (!vma ||
		vma->vm_start != start ||
		!vma->vm_swmmu_pt_range) {
		ret = -EINVAL;
		goto out;
	}

	length = vma->vm_end - vma->vm_start;
	ret = do_munmap(mm, start, length, NULL);

out:
	mmap_write_unlock(mm);
	return ret;
}

SYSCALL_DEFINE4(nommu_swmmu_load,
		void __user *, address,
		size_t, size,
		u64 __user *, result,
		unsigned int, flags)
{
	u64 value = 0;
	int ret;
	bool return_value = flags & NOMMU_SWMMU_ACCESS_RETURN_VALUE;

	if (flags == NOMMU_SWMMU_QUERY_DIRECT_WINDOW) {
		struct nommu_swmmu_space *space = current->mm->swmmu_space;
		unsigned long start, end;
		u64 bounds[2];

		if (address || size != sizeof(bounds) || !result)
			return -EINVAL;
		if (!space || !space->host_ops ||
		    !space->host_ops->direct_window)
			return -EOPNOTSUPP;
		ret = space->host_ops->direct_window(&start, &end);
		if (ret)
			return ret;
		bounds[0] = start;
		bounds[1] = end;
		return copy_to_user(result, bounds, sizeof(bounds)) ? -EFAULT : 0;
	}

	if (flags & ~NOMMU_SWMMU_ACCESS_MASK)
		return -EINVAL;
	if (return_value &&
	    (!(flags & NOMMU_SWMMU_ACCESS_SIGNAL) || result))
		return -EINVAL;
	if (return_value && sizeof(long) < sizeof(value))
		return -EOPNOTSUPP;
	if (!return_value && !result)
		return -EFAULT;

	if (!swmmu_valid_access_size(size))
		return -EINVAL;

	if (flags & NOMMU_SWMMU_ACCESS_DYNAMIC) {
		ret = swmmu_dynamic_copy_from_mm(
			current->mm, (unsigned long)address,
			&value, size);
	} else {
		ret = nommu_swmmu_load_u64_checked(
			address, size, &value);
	}

	if (ret && return_value) {
		force_sig_fault(SIGSEGV,
				ret == -EACCES ? SEGV_ACCERR : SEGV_MAPERR,
				(void __user *)address);
		return -ERESTARTNOINTR;
	}
	if (ret)
		return swmmu_signal_access_error(address, ret, flags);
	if (return_value)
		return (long)value;

	if (copy_to_user(result, &value, sizeof(value)))
		return -EFAULT;

	return 0;
}

SYSCALL_DEFINE4(nommu_swmmu_store,
		void __user *, address,
		size_t, size,
		uint64_t, value,
		unsigned int, flags)
{
	int ret;

	if (flags & ~NOMMU_SWMMU_ACCESS_MASK)
		return -EINVAL;
	if (flags & NOMMU_SWMMU_ACCESS_RETURN_VALUE)
		return -EINVAL;

	if (!swmmu_valid_access_size(size))
		return -EINVAL;

	if (flags & NOMMU_SWMMU_ACCESS_DYNAMIC) {
		ret = swmmu_dynamic_copy_to_mm(
			current->mm, (unsigned long)address,
			&value, size);
	} else {
		ret = nommu_swmmu_store_u64_checked(
			address, size, value);
	}

	return swmmu_signal_access_error(address, ret, flags);
}

static int swmmu_prepare_mprotect_range(struct mm_struct *mm,
					unsigned long start,
					unsigned long end,
					unsigned long prot)
{
	VMA_ITERATOR(vmi, mm, start);
	struct vm_area_struct *vma;
	int ret;

	vma = vma_find(&vmi, end);
	if (!vma)
		return -EOPNOTSUPP;
	if (!vma->vm_swmmu_pt_range)
		return -ENOSYS;

	if (start < vma->vm_start || end > vma->vm_end)
		return -EINVAL;

	if (start > vma->vm_start) {
		ret = vma_split_backend(&vmi, vma, start, false,
					&swmmu_vma_mapping_ops);
		if (ret)
			return ret;
	}

	vma_iter_set(&vmi, start);
	vma = vma_find(&vmi, end);
	if (!vma)
		return -EINVAL;

	if (end < vma->vm_end) {
		ret = vma_split_backend(&vmi, vma, end, true,
					&swmmu_vma_mapping_ops);
		if (ret)
			return ret;
	}

	return 0;
}

SYSCALL_DEFINE3(mprotect, unsigned long, start, size_t, len,
		unsigned long, prot)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct swmmu_pagetable_range *range;
	VMA_ITERATOR(vmi, mm, start);
	unsigned long end;
	int ret;

	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
		return -EINVAL;

	if (!len || len > ULONG_MAX - start)
		return -EINVAL;

	len = PAGE_ALIGN(len);
	end = start + len;

	mmap_write_lock(mm);

	ret = swmmu_prepare_mprotect_range(mm, start, end, prot);
	if (ret)
		goto out_unlock;

	vma_iter_set(&vmi, start);
	vma = vma_find(&vmi, end);
	if (!vma ||
	    vma->vm_start != start ||
	    vma->vm_end != end) {
		ret = -EINVAL;
		goto out_unlock;
	}

	range = vma->vm_swmmu_pt_range;
	if (!range) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}

	ret = swmmu_host_protect_range(mm->swmmu_space, range, prot);
	if (ret)
		goto out_unlock;

	range->prot = prot;

out_unlock:
	mmap_write_unlock(mm);
	return ret;
}

SYSCALL_DEFINE5(nommu_swmmu_cmpxchg,
		void __user *, address,
		u32, expected,
		u32, desired,
		u32 __user *, observed,
		unsigned int, flags)
{
	u32 old;
	int ret;

	if (flags & ~NOMMU_SWMMU_ACCESS_MASK)
		return -EINVAL;
	if (flags & NOMMU_SWMMU_ACCESS_RETURN_VALUE)
		return -EINVAL;

	if (!(flags & NOMMU_SWMMU_ACCESS_DYNAMIC))
		return -EOPNOTSUPP;

	ret = swmmu_dynamic_cmpxchg_u32_mm(
		current->mm,
		(unsigned long)address,
		expected,
		desired,
		&old);
	if (ret)
		return swmmu_signal_access_error(address, ret, flags);

	if (copy_to_user(observed, &old, sizeof(old)))
		return -EFAULT;

	return 0;
}
