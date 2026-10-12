/*
 * drivers/staging/android/ion/ion_system_heap.c
 *
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/compat.h>
#include <asm/page.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <asm/tlbflush.h>
#include "ion.h"
#include "ion_priv.h"

static gfp_t high_order_gfp_flags = (GFP_HIGHUSER | __GFP_ZERO | __GFP_NOWARN |
				     __GFP_NORETRY) & ~__GFP_RECLAIM;
/* ion-bp: order 0 keeps full reclaim but no longer suppresses the
 * failure report - mainline d2805d7fff5b (v4.9): "useful to show the
 * current memory in detail when alloc failed ... just show memory when an
 * order 0 alloc failed".  AOSP's 2025 __GFP_RETRY_MAYFAIL addition has no
 * 4.4 counterpart (the flag does not exist this far back - verified in
 * include/linux/gfp.h), so 4.4's default order-0 reclaim policy is used
 * as-is; the missing half was the visibility. */
static gfp_t low_order_gfp_flags  = (GFP_HIGHUSER | __GFP_ZERO);
static const unsigned int orders[] = {8, 4, 0};
static const int num_orders = ARRAY_SIZE(orders);
static unsigned int fixed_max_order;
static unsigned long ion_system_heap_budget;
static struct ion_system_heap *system_heap;

static int order_to_index(unsigned int order)
{
	int i;

	for (i = 0; i < num_orders; i++)
		if (order == orders[i])
			return i;
	BUG();
	return -1;
}

static inline unsigned int order_to_size(int order)
{
	return PAGE_SIZE << order;
}

struct ion_system_heap {
	struct ion_heap heap;
	struct ion_page_pool **pools;
};

static struct page *alloc_buffer_page(struct ion_system_heap *heap,
				      struct ion_buffer *buffer,
				      unsigned long order)
{
	int idx = order_to_index(order);
	struct ion_page_pool *pool;
	struct page *page;

	if (!ion_buffer_cached(buffer))
		idx += num_orders;

	pool = heap->pools[idx];

	page = ion_page_pool_alloc(pool);
	if (!page) {
		/* try with alternative pool */
		if (ion_buffer_cached(buffer))
			pool = heap->pools[idx + num_orders];
		else
			pool = heap->pools[idx - num_orders];

		page = ion_page_pool_alloc(pool);
	}

	if (!page)
		page = ion_page_pool_alloc_pages(pool);

	return page;
}

static void free_buffer_page(struct ion_system_heap *heap,
			     struct ion_buffer *buffer, struct page *page)
{
	unsigned int order = compound_order(page);

	struct ion_page_pool *pool = heap->pools[order_to_index(order)];
	if (buffer->private_flags & ION_PRIV_FLAG_SHRINKER_FREE) {
		ion_page_pool_free_immediate(pool, page);
	} else {
		int uncached = ion_buffer_cached(buffer) ? 0 : 1;
		int idx = order_to_index(order) + (num_orders * uncached);
		struct ion_page_pool *pool = heap->pools[idx];

		ion_page_pool_free(pool, page);
	}
}


static struct page *alloc_largest_available(struct ion_system_heap *heap,
					    struct ion_buffer *buffer,
					    unsigned long size,
					    unsigned int max_order)
{
	struct page *page;
	int i;

	for (i = 0; i < num_orders; i++) {
		if (size < order_to_size(orders[i]))
			continue;
		if (max_order < orders[i])
			continue;

		page = alloc_buffer_page(heap, buffer, orders[i]);
		if (!page)
			continue;

		return page;
	}

	return NULL;
}

#define should_flush_cache(page, buffer) (!ion_get_page_clean(page) &&	\
		(!ion_buffer_cached(buffer) || ion_buffer_sync_force(buffer)))

static int ion_system_heap_allocate(struct ion_heap *heap,
				     struct ion_buffer *buffer,
				     unsigned long size, unsigned long align,
				     unsigned long flags)
{
	struct ion_system_heap *sys_heap = container_of(heap,
							struct ion_system_heap,
							heap);
	struct sg_table *table;
	struct scatterlist *sg;
	struct list_head pages;
	struct page *page, *tmp_page;
	int i = 0;
	unsigned long size_remaining = PAGE_ALIGN(size);
	unsigned int max_order = fixed_max_order;

	if (align > PAGE_SIZE)
		return -EINVAL;

	if (size / PAGE_SIZE > totalram_pages() / 2)
		return -ENOMEM;

	/* ion-bp: aggregate system-heap budget.  Upstream ION only ever had
	 * the per-allocation half-RAM guard above (c9e8440eca61); ACK tracked
	 * a heap-wide total_heap_bytes counter but stats-only, never enforced.
	 * We reuse heap->total_allocated, which ion.c maintains atomically
	 * (added in ion_buffer_create on success, subtracted in
	 * ion_buffer_destroy; failed allocates never increment it and never
	 * reach heap->ops->free), so this needs no free-path pairing.
	 * Best effort only - concurrent allocators can each win the check,
	 * exactly like the guard above. */
	if (ion_system_heap_budget &&
	    (atomic_long_read(&heap->total_allocated) + size >
	     ion_system_heap_budget)) {
		pr_info_ratelimited("ion: system-heap budget %lu kB reached (allocated %lu kB, want %lu kB)\n",
				    ion_system_heap_budget >> 10,
				    atomic_long_read(&heap->total_allocated) >> 10,
				    size >> 10);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&pages);
	while (size_remaining > 0) {
		/* ion-bp: stop allocating for a task that is being SIGKILLed
		 * (dma-buf heaps, v6.1 system_heap_allocate): during an LMK
		 * burst a killed process would otherwise keep grinding direct
		 * reclaim inside this loop.  free_pages below already unwinds
		 * the partially built page list. */
		if (fatal_signal_pending(current))
			goto free_pages;

		page = alloc_largest_available(sys_heap, buffer, size_remaining,
						max_order);
		if (!page)
			goto free_pages;
		list_add_tail(&page->lru, &pages);
		size_remaining -= PAGE_SIZE << compound_order(page);
		max_order = compound_order(page);
		i++;
	}
	table = kmalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (!table)
		goto free_pages;

	if (sg_alloc_table(table, i, GFP_KERNEL))
		goto free_table;

	sg = table->sgl;
	list_for_each_entry_safe(page, tmp_page, &pages, lru) {
		unsigned int len = PAGE_SIZE << compound_order(page);
		sg_set_page(sg, page, len, 0);
		sg = sg_next(sg);
		if (should_flush_cache(page, buffer)) {
			__flush_dcache_area(page_address(page), len);
			if (!ion_buffer_cached(buffer))
				ion_set_page_clean(page);
		}
		list_del(&page->lru);
	}

	buffer->priv_virt = table;
	return 0;

free_table:
	kfree(table);
free_pages:
	list_for_each_entry_safe(page, tmp_page, &pages, lru) {
		list_del(&page->lru);
		buffer->private_flags |= ION_PRIV_FLAG_SHRINKER_FREE;
		free_buffer_page(sys_heap, buffer, page);
	}

	return -ENOMEM;
}

static void ion_system_heap_free(struct ion_buffer *buffer)
{
	struct ion_system_heap *sys_heap = container_of(buffer->heap,
							struct ion_system_heap,
							heap);
	struct sg_table *table = buffer->sg_table;
	struct scatterlist *sg;
	int i;

	/*
	 *  pages come from the page pools, zero them before returning
	 *  for security purposes (other allocations are zerod at
	 *  alloc time
	 */
	if (!(buffer->private_flags & ION_PRIV_FLAG_SHRINKER_FREE))
		ion_heap_buffer_zero(buffer);

	for_each_sg(table->sgl, sg, table->nents, i)
		free_buffer_page(sys_heap, buffer, sg_page(sg));
	sg_free_table(table);
	kfree(table);
}

static struct sg_table *ion_system_heap_map_dma(struct ion_heap *heap,
						struct ion_buffer *buffer)
{
	return buffer->priv_virt;
}

static void ion_system_heap_unmap_dma(struct ion_heap *heap,
				      struct ion_buffer *buffer)
{
}

static int ion_system_heap_shrink(struct ion_heap *heap, gfp_t gfp_mask,
					int nr_to_scan)
{
	struct ion_system_heap *sys_heap;
	int nr_total = 0;
	int i, nr_freed, nocached;
	int only_scan = 0;

	sys_heap = container_of(heap, struct ion_system_heap, heap);

	if (!nr_to_scan)
		only_scan = 1;

	for (nocached = 0; nocached < 2; nocached++) {
		for (i = 0; i < num_orders; i++) {
			struct ion_page_pool *pool =
				sys_heap->pools[i + num_orders * nocached];
			nr_freed = ion_page_pool_shrink(pool, gfp_mask, nr_to_scan);
			nr_total += nr_freed;

			if (!only_scan) {
				nr_to_scan -= nr_freed;
				/* shrink completed */
				if (nr_to_scan <= 0)
					break;
			}
		}
	}

	return nr_total;
}

static struct ion_heap_ops system_heap_ops = {
	.allocate = ion_system_heap_allocate,
	.free = ion_system_heap_free,
	.map_dma = ion_system_heap_map_dma,
	.unmap_dma = ion_system_heap_unmap_dma,
	.map_kernel = ion_heap_map_kernel,
	.unmap_kernel = ion_heap_unmap_kernel,
	.map_user = ion_heap_map_user,
	.shrink = ion_system_heap_shrink,
};

static int ion_system_heap_debug_show(struct ion_heap *heap, struct seq_file *s,
				      void *unused)
{

	struct ion_system_heap *sys_heap = container_of(heap,
							struct ion_system_heap,
							heap);
	int i;

	for (i = 0; i < num_orders; i++) {
		struct ion_page_pool *pool = sys_heap->pools[i];

		seq_printf(s, "%d order %u highmem pages in cached pool = %lu total\n",
			   pool->high_count, pool->order,
			   (PAGE_SIZE << pool->order) * pool->high_count);
		seq_printf(s, "%d order %u lowmem pages in cached pool = %lu total\n",
			   pool->low_count, pool->order,
			   (PAGE_SIZE << pool->order) * pool->low_count);
	}

	for (i = num_orders; i < (num_orders * 2); i++) {
		struct ion_page_pool *pool = sys_heap->pools[i];

		seq_printf(s, "%d order %u highmem pages in uncached pool = %lu total\n",
			   pool->high_count, pool->order,
			   (PAGE_SIZE << pool->order) * pool->high_count);
		seq_printf(s, "%d order %u lowmem pages in uncached pool = %lu total\n",
			   pool->low_count, pool->order,
			   (PAGE_SIZE << pool->order) * pool->low_count);
	}

	return 0;
}

void show_ion_system_heap_size(struct seq_file *s)
{
	struct ion_heap *heap;
	unsigned long system_byte = 0;

	if (!system_heap) {
		pr_err("system_heap is not ready\n");
		return;
	}

	heap = &system_heap->heap;
	system_byte = (unsigned long)atomic_long_read(&heap->total_allocated);
	if (s)
		seq_printf(s, "SystemHeap:     %8lu kB\n", system_byte >> 10);
	else
		pr_cont("SystemHeap:%lukB ", system_byte >> 10);
}

void show_ion_system_heap_pool_size(struct seq_file *s)
{
	unsigned long pool_size = 0;
	struct ion_page_pool *pool;
	int i;

	if (!system_heap) {
		pr_err("system_heap_pool is not ready\n");
		return;
	}

	for (i = 0; i < num_orders * 2; i++) {
		pool = system_heap->pools[i];
		pool_size += (1 << pool->order) * pool->high_count;
		pool_size += (1 << pool->order) * pool->low_count;
	}

	if (s)
		seq_printf(s, "SystemHeapPool: %8lu kB\n",
			   pool_size << (PAGE_SHIFT - 10));
	else
		pr_cont("SystemHeapPool:%lukB ",
			pool_size << (PAGE_SHIFT - 10));
}

static ssize_t ion_system_heap_orders_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", fixed_max_order);
}

static ssize_t ion_system_heap_orders_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t n)
{
	int ret, i;
	unsigned int order;
	ret = sscanf(buf, "%u", &order);
	if (ret == 1) {
		for (i = 0; i < num_orders; i++) {
			if (orders[i] == order) {
				fixed_max_order = order;
				return n;
			}
		}
	}
	return -EINVAL;
}

static ssize_t ion_system_heap_budget_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ion_system_heap_budget >> 10);
}

static ssize_t ion_system_heap_budget_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t n)
{
	unsigned long budget;
	int ret;

	ret = kstrtoul(buf, 0, &budget);
	if (ret)
		return ret;
	ion_system_heap_budget = budget << 10;
	return n;
}

static struct kobj_attribute ion_system_heap_budget_attr =
	__ATTR(ion_system_heap_budget, 0644,
		ion_system_heap_budget_show, ion_system_heap_budget_store);

static struct kobj_attribute ion_system_heap_orders_attr =
	__ATTR(ion_system_heap_orders, 0644,
		ion_system_heap_orders_show, ion_system_heap_orders_store);

struct ion_heap *ion_system_heap_create(struct ion_platform_heap *unused)
{
	struct ion_system_heap *heap;
	int i;

	heap = kzalloc(sizeof(struct ion_system_heap), GFP_KERNEL);
	if (!heap)
		return ERR_PTR(-ENOMEM);
	heap->heap.ops = &system_heap_ops;
	heap->heap.type = ION_HEAP_TYPE_SYSTEM;
	heap->heap.flags = ION_HEAP_FLAG_DEFER_FREE;
	heap->pools = kzalloc(sizeof(struct ion_page_pool *) * num_orders * 2,
			      GFP_KERNEL);
	if (!heap->pools)
		goto free_heap;
	for (i = 0; i < num_orders * 2; i++) {
		struct ion_page_pool *pool;
		gfp_t gfp_flags = low_order_gfp_flags;

		if (orders[i % num_orders] > 0)
			gfp_flags = high_order_gfp_flags;
		pool = ion_page_pool_create(gfp_flags, orders[i % num_orders]);
		if (!pool)
			goto destroy_pools;
		pool->cached = i < num_orders ? true : false;
		heap->pools[i] = pool;
	}

	heap->heap.debug_show = ion_system_heap_debug_show;
	fixed_max_order = orders[0];

	/* ion-bp: aggregate budget default = totalram/2, the same ratio the
	 * single-allocation guard uses.  On this 2.7 GB device that is
	 * ~1.35 GB - deliberately above the measured Night-Sight peak of
	 * 1,185,684 kB (5x7.5Mpx raw + pipeline), so it contains runaway
	 * growth without capping the camera workload itself. */
	ion_system_heap_budget = (unsigned long)totalram_pages() * PAGE_SIZE / 2;
	if (sysfs_create_file(kernel_kobj, &ion_system_heap_budget_attr.attr))
		pr_err("%s: Failed to create sysfs on ION system heap budget", __func__);
	if (sysfs_create_file(kernel_kobj, &ion_system_heap_orders_attr.attr))
		pr_err("%s: Failed to create sysfs on ION system heap", __func__);

	if (!system_heap)
		system_heap = heap;
	else
		pr_err("system_heap had been already created\n");

	return &heap->heap;

destroy_pools:
	while (i--)
		ion_page_pool_destroy(heap->pools[i]);
	kfree(heap->pools);
free_heap:
	kfree(heap);
	return ERR_PTR(-ENOMEM);
}

void ion_system_heap_destroy(struct ion_heap *heap)
{
	struct ion_system_heap *sys_heap = container_of(heap,
							struct ion_system_heap,
							heap);
	int i;

	for (i = 0; i < num_orders * 2; i++)
		ion_page_pool_destroy(sys_heap->pools[i]);
	kfree(sys_heap->pools);
	kfree(sys_heap);
}

static int ion_system_contig_heap_allocate(struct ion_heap *heap,
					   struct ion_buffer *buffer,
					   unsigned long len,
					   unsigned long align,
					   unsigned long flags)
{
	int order = get_order(len);
	struct page *page;
	struct sg_table *table;
	unsigned long i;
	int ret;

	if (align > (PAGE_SIZE << order))
		return -EINVAL;

	page = alloc_pages(low_order_gfp_flags, order);
	if (!page)
		return -ENOMEM;

	split_page(page, order);

	len = PAGE_ALIGN(len);
	for (i = len >> PAGE_SHIFT; i < (1 << order); i++)
		__free_page(page + i);

	table = kmalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (!table) {
		ret = -ENOMEM;
		goto free_pages;
	}

	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret)
		goto free_table;

	sg_set_page(table->sgl, page, len, 0);

	buffer->priv_virt = table;

	ion_pages_sync_for_device(NULL, page, len, DMA_BIDIRECTIONAL);

	return 0;

free_table:
	kfree(table);
free_pages:
	for (i = 0; i < len >> PAGE_SHIFT; i++)
		__free_page(page + i);

	return ret;
}

static void ion_system_contig_heap_free(struct ion_buffer *buffer)
{
	struct sg_table *table = buffer->priv_virt;
	struct page *page = sg_page(table->sgl);
	unsigned long pages = PAGE_ALIGN(buffer->size) >> PAGE_SHIFT;
	unsigned long i;

	for (i = 0; i < pages; i++)
		__free_page(page + i);
	sg_free_table(table);
	kfree(table);
}

static int ion_system_contig_heap_phys(struct ion_heap *heap,
				       struct ion_buffer *buffer,
				       ion_phys_addr_t *addr, size_t *len)
{
	struct sg_table *table = buffer->priv_virt;
	struct page *page = sg_page(table->sgl);
	*addr = page_to_phys(page);
	*len = buffer->size;
	return 0;
}

static struct sg_table *ion_system_contig_heap_map_dma(struct ion_heap *heap,
						struct ion_buffer *buffer)
{
	return buffer->priv_virt;
}

static void ion_system_contig_heap_unmap_dma(struct ion_heap *heap,
					     struct ion_buffer *buffer)
{
}

static struct ion_heap_ops kmalloc_ops = {
	.allocate = ion_system_contig_heap_allocate,
	.free = ion_system_contig_heap_free,
	.phys = ion_system_contig_heap_phys,
	.map_dma = ion_system_contig_heap_map_dma,
	.unmap_dma = ion_system_contig_heap_unmap_dma,
	.map_kernel = ion_heap_map_kernel,
	.unmap_kernel = ion_heap_unmap_kernel,
	.map_user = ion_heap_map_user,
};

struct ion_heap *ion_system_contig_heap_create(struct ion_platform_heap *unused)
{
	struct ion_heap *heap;

	heap = kzalloc(sizeof(struct ion_heap), GFP_KERNEL);
	if (!heap)
		return ERR_PTR(-ENOMEM);
	heap->ops = &kmalloc_ops;
	heap->type = ION_HEAP_TYPE_SYSTEM_CONTIG;
	return heap;
}

void ion_system_contig_heap_destroy(struct ion_heap *heap)
{
	kfree(heap);
}
