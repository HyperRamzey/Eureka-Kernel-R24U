/*
 *  linux/mm/vmscan.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/vmpressure.h>
/*
 * Included unconditionally: <linux/psi.h> supplies no-op stubs for
 * psi_memstall_enter/leave when CONFIG_PSI=n, so the reclaim hooks
 * below need no #ifdef and cost nothing when PSI is off.
 */
#include <linux/psi.h>
#include <linux/vmstat.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* for try_to_release_page(),
					buffer_heads_over_limit */
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/compaction.h>
#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/memcontrol.h>
#include <linux/delayacct.h>
#include <linux/sysctl.h>
#include <linux/oom.h>
#include <linux/prefetch.h>
#include <linux/printk.h>
#include <linux/pagewalk.h>
#include <linux/pagevec.h>
#include <linux/huge_mm.h>
#include <linux/debugfs.h>
#include <linux/ctype.h>
#include <linux/cgroup.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>

#include <linux/swapops.h>
#include <linux/balloon_compaction.h>

#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/vmscan.h>

struct scan_control {
	/* How many pages shrink_list() should reclaim */
	unsigned long nr_to_reclaim;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	/* Allocation order */
	int order;

	/*
	 * Nodemask of nodes allowed by the caller. If NULL, all nodes
	 * are scanned.
	 */
	nodemask_t	*nodemask;

	/*
	 * The memory cgroup that hit its limit and as a result is the
	 * primary target of this reclaim invocation.
	 */
	struct mem_cgroup *target_mem_cgroup;

	int swappiness;

	/* Scan (total_size >> priority) pages at once */
	int priority;

	unsigned int may_writepage:1;

	/* Can mapped pages be reclaimed? */
	unsigned int may_unmap:1;

	/* Can pages be swapped as part of reclaim? */
	unsigned int may_swap:1;

	/* Can cgroups be reclaimed below their normal consumption range? */
	unsigned int may_thrash:1;

	unsigned int hibernation_mode:1;

	/* One of the zones is ready for compaction */
	unsigned int compaction_ready:1;

	/* Incremented by the number of inactive pages that were scanned */
	unsigned long nr_scanned;

	/* Number of pages freed so far during a call to shrink_zones() */
	unsigned long nr_reclaimed;
	
	/*
	 * Reclaim pages from a vma. If the page is shared by other tasks
	 * it is zapped from a vma without reclaim so it ends up remaining
	 * on memory until last task zap it.
	 */
	struct vm_area_struct *target_vma;

#ifdef CONFIG_LRU_GEN
	/* help kswapd make better choices among multiple memcgs */
	unsigned int memcgs_need_aging:1;
	unsigned long last_reclaimed;
	/* 4.4: read by the region's low-protection gate; never set here */
	unsigned int memcg_low_reclaim:1;
	/* mm walker state owned by this reclaim run */
	struct reclaim_state reclaim_state;
#endif
};

#ifdef ARCH_HAS_PREFETCH
#define prefetch_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetch(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetch_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

/*
 * From 0 .. 100.  Higher means more swappy.
 */
int vm_swappiness = 100;
/*
 * The total number of pages which are beyond the high watermark within all
 * zones.
 */
unsigned long vm_total_pages;

static LIST_HEAD(shrinker_list);
static DECLARE_RWSEM(shrinker_rwsem);

#ifdef CONFIG_MEMCG
static bool global_reclaim(struct scan_control *sc)
{
	return !sc->target_mem_cgroup;
}

/**
 * sane_reclaim - is the usual dirty throttling mechanism operational?
 * @sc: scan_control in question
 *
 * The normal page dirty throttling mechanism in balance_dirty_pages() is
 * completely broken with the legacy memcg and direct stalling in
 * shrink_page_list() is used for throttling instead, which lacks all the
 * niceties such as fairness, adaptive pausing, bandwidth proportional
 * allocation and configurability.
 *
 * This function tests whether the vmscan currently in progress can assume
 * that the normal dirty throttling mechanism is operational.
 */
static bool sane_reclaim(struct scan_control *sc)
{
	struct mem_cgroup *memcg = sc->target_mem_cgroup;

	if (!memcg)
		return true;
#ifdef CONFIG_CGROUP_WRITEBACK
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return true;
#endif
	return false;
}
#else
static bool global_reclaim(struct scan_control *sc)
{
	return true;
}

static bool sane_reclaim(struct scan_control *sc)
{
	return true;
}
#endif

static unsigned long zone_reclaimable_pages(struct zone *zone)
{
	unsigned long nr;

	nr = zone_page_state(zone, NR_ACTIVE_FILE) +
	     zone_page_state(zone, NR_INACTIVE_FILE);

	if (get_nr_swap_pages() > 0)
		nr += zone_page_state(zone, NR_ACTIVE_ANON) +
		      zone_page_state(zone, NR_INACTIVE_ANON);

	return nr;
}

bool zone_reclaimable(struct zone *zone)
{
	return zone_page_state(zone, NR_PAGES_SCANNED) <
		zone_reclaimable_pages(zone) * 6;
}

static unsigned long get_lru_size(struct lruvec *lruvec, enum lru_list lru)
{
	if (!mem_cgroup_disabled())
		return mem_cgroup_get_lru_size(lruvec, lru);

	return zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru);
}

/*
 * Add a shrinker callback to be called from the vm.
 */
int register_shrinker(struct shrinker *shrinker)
{
	size_t size = sizeof(*shrinker->nr_deferred);

	/*
	 * If we only have one possible node in the system anyway, save
	 * ourselves the trouble and disable NUMA aware behavior. This way we
	 * will save memory and some small loop time later.
	 */
	if (nr_node_ids == 1)
		shrinker->flags &= ~SHRINKER_NUMA_AWARE;

	if (shrinker->flags & SHRINKER_NUMA_AWARE)
		size *= nr_node_ids;

	shrinker->nr_deferred = kzalloc(size, GFP_KERNEL);
	if (!shrinker->nr_deferred)
		return -ENOMEM;

	down_write(&shrinker_rwsem);
	list_add_tail(&shrinker->list, &shrinker_list);
	up_write(&shrinker_rwsem);
	return 0;
}
EXPORT_SYMBOL(register_shrinker);

/*
 * Remove one
 */
void unregister_shrinker(struct shrinker *shrinker)
{
	if (!shrinker->nr_deferred)
		return;
	down_write(&shrinker_rwsem);
	list_del(&shrinker->list);
	up_write(&shrinker_rwsem);
	kfree(shrinker->nr_deferred);
	shrinker->nr_deferred = NULL;
}
EXPORT_SYMBOL(unregister_shrinker);

#define SHRINK_BATCH 128

static unsigned long do_shrink_slab(struct shrink_control *shrinkctl,
				    struct shrinker *shrinker,
				    unsigned long nr_scanned,
				    unsigned long nr_eligible)
{
	unsigned long freed = 0;
	unsigned long long delta;
	long total_scan;
	long freeable;
	long nr;
	long new_nr;
	int nid = shrinkctl->nid;
	long batch_size = shrinker->batch ? shrinker->batch
					  : SHRINK_BATCH;
	long scanned = 0, next_deferred;

	freeable = shrinker->count_objects(shrinker, shrinkctl);
	if (freeable == 0)
		return 0;

	/*
	 * copy the current shrinker scan count into a local variable
	 * and zero it so that other concurrent shrinker invocations
	 * don't also do this scanning work.
	 */
	nr = atomic_long_xchg(&shrinker->nr_deferred[nid], 0);

	total_scan = nr;
	delta = (4 * nr_scanned) / shrinker->seeks;
	delta *= freeable;
	do_div(delta, nr_eligible + 1);
	total_scan += delta;
	if (total_scan < 0) {
		pr_err("shrink_slab: %pF negative objects to delete nr=%ld\n",
		       shrinker->scan_objects, total_scan);
		total_scan = freeable;
		next_deferred = nr;
	} else
		next_deferred = total_scan;

	/*
	 * We need to avoid excessive windup on filesystem shrinkers
	 * due to large numbers of GFP_NOFS allocations causing the
	 * shrinkers to return -1 all the time. This results in a large
	 * nr being built up so when a shrink that can do some work
	 * comes along it empties the entire cache due to nr >>>
	 * freeable. This is bad for sustaining a working set in
	 * memory.
	 *
	 * Hence only allow the shrinker to scan the entire cache when
	 * a large delta change is calculated directly.
	 */
	if (delta < freeable / 4)
		total_scan = min(total_scan, freeable / 2);

	/*
	 * Avoid risking looping forever due to too large nr value:
	 * never try to free more than twice the estimate number of
	 * freeable entries.
	 */
	if (total_scan > freeable * 2)
		total_scan = freeable * 2;

	trace_mm_shrink_slab_start(shrinker, shrinkctl, nr,
				   nr_scanned, nr_eligible,
				   freeable, delta, total_scan);

	/*
	 * Normally, we should not scan less than batch_size objects in one
	 * pass to avoid too frequent shrinker calls, but if the slab has less
	 * than batch_size objects in total and we are really tight on memory,
	 * we will try to reclaim all available objects, otherwise we can end
	 * up failing allocations although there are plenty of reclaimable
	 * objects spread over several slabs with usage less than the
	 * batch_size.
	 *
	 * We detect the "tight on memory" situations by looking at the total
	 * number of objects we want to scan (total_scan). If it is greater
	 * than the total number of objects on slab (freeable), we must be
	 * scanning at high prio and therefore should try to reclaim as much as
	 * possible.
	 */
	while (total_scan >= batch_size ||
	       total_scan >= freeable) {
		unsigned long ret;
		unsigned long nr_to_scan = min(batch_size, total_scan);

		shrinkctl->nr_to_scan = nr_to_scan;
		ret = shrinker->scan_objects(shrinker, shrinkctl);
		if (ret == SHRINK_STOP)
			break;
		freed += ret;

		count_vm_events(SLABS_SCANNED, nr_to_scan);
		total_scan -= nr_to_scan;
		scanned += nr_to_scan;

		cond_resched();
	}

	if (next_deferred >= scanned)
		next_deferred -= scanned;
	else
		next_deferred = 0;
	/*
	 * move the unused scan count back into the shrinker in a
	 * manner that handles concurrent updates. If we exhausted the
	 * scan, there is no need to do an update.
	 */
	if (next_deferred > 0)
		new_nr = atomic_long_add_return(next_deferred,
						&shrinker->nr_deferred[nid]);
	else
		new_nr = atomic_long_read(&shrinker->nr_deferred[nid]);

	trace_mm_shrink_slab_end(shrinker, nid, freed, nr, new_nr, total_scan);
	return freed;
}

/**
 * shrink_slab - shrink slab caches
 * @gfp_mask: allocation context
 * @nid: node whose slab caches to target
 * @memcg: memory cgroup whose slab caches to target
 * @nr_scanned: pressure numerator
 * @nr_eligible: pressure denominator
 *
 * Call the shrink functions to age shrinkable caches.
 *
 * @nid is passed along to shrinkers with SHRINKER_NUMA_AWARE set,
 * unaware shrinkers will receive a node id of 0 instead.
 *
 * @memcg specifies the memory cgroup to target. If it is not NULL,
 * only shrinkers with SHRINKER_MEMCG_AWARE set will be called to scan
 * objects from the memory cgroup specified. Otherwise all shrinkers
 * are called, and memcg aware shrinkers are supposed to scan the
 * global list then.
 *
 * @nr_scanned and @nr_eligible form a ratio that indicate how much of
 * the available objects should be scanned.  Page reclaim for example
 * passes the number of pages scanned and the number of pages on the
 * LRU lists that it considered on @nid, plus a bias in @nr_scanned
 * when it encountered mapped pages.  The ratio is further biased by
 * the ->seeks setting of the shrink function, which indicates the
 * cost to recreate an object relative to that of an LRU page.
 *
 * Returns the number of reclaimed slab objects.
 */
static unsigned long shrink_slab(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg,
				 unsigned long nr_scanned,
				 unsigned long nr_eligible)
{
	struct shrinker *shrinker;
	unsigned long freed = 0;

	if (memcg && !memcg_kmem_is_active(memcg))
		return 0;

	if (nr_scanned == 0)
		nr_scanned = SWAP_CLUSTER_MAX;

	if (!down_read_trylock(&shrinker_rwsem)) {
		/*
		 * If we would return 0, our callers would understand that we
		 * have nothing else to shrink and give up trying. By returning
		 * 1 we keep it going and assume we'll be able to shrink next
		 * time.
		 */
		freed = 1;
		goto out;
	}

	list_for_each_entry(shrinker, &shrinker_list, list) {
		struct shrink_control sc = {
			.gfp_mask = gfp_mask,
			.nid = nid,
			.memcg = memcg,
		};

		if (memcg && !(shrinker->flags & SHRINKER_MEMCG_AWARE))
			continue;

		if (!(shrinker->flags & SHRINKER_NUMA_AWARE))
			sc.nid = 0;

		freed += do_shrink_slab(&sc, shrinker, nr_scanned, nr_eligible);
	}

	up_read(&shrinker_rwsem);
out:
	cond_resched();
	return freed;
}

void drop_slab_node(int nid)
{
	unsigned long freed;

	do {
		struct mem_cgroup *memcg = NULL;

		freed = 0;
		do {
			freed += shrink_slab(GFP_KERNEL, nid, memcg,
					     1000, 1000);
		} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)) != NULL);
	} while (freed > 10);
}

void drop_slab(void)
{
	int nid;

	for_each_online_node(nid)
		drop_slab_node(nid);
}

static inline int is_page_cache_freeable(struct page *page)
{
	/*
	 * A freeable page cache page is referenced only by the caller
	 * that isolated the page, the page cache radix tree and
	 * optional buffer heads at page->private.
	 */
	return page_count(page) - page_has_private(page) == 2;
}

static int may_write_to_inode(struct inode *inode)
{
	if (current->flags & PF_SWAPWRITE)
		return 1;
	if (!inode_write_congested(inode))
		return 1;
	if (inode_to_bdi(inode) == current->backing_dev_info)
		return 1;
	return 0;
}

/*
 * We detected a synchronous write error writing a page out.  Probably
 * -ENOSPC.  We need to propagate that into the address_space for a subsequent
 * fsync(), msync() or close().
 *
 * The tricky part is that after writepage we cannot touch the mapping: nothing
 * prevents it from being freed up.  But we have a ref on the page and once
 * that page is locked, the mapping is pinned.
 *
 * We're allowed to run sleeping lock_page() here because we know the caller has
 * __GFP_FS.
 */
static void handle_write_error(struct address_space *mapping,
				struct page *page, int error)
{
	lock_page(page);
	if (page_mapping(page) == mapping)
		mapping_set_error(mapping, error);
	unlock_page(page);
}

/* possible outcome of pageout() */
typedef enum {
	/* failed to write page out, page is locked */
	PAGE_KEEP,
	/* move page to the active list, page is locked */
	PAGE_ACTIVATE,
	/* page has been sent to the disk successfully, page is unlocked */
	PAGE_SUCCESS,
	/* page is clean and locked */
	PAGE_CLEAN,
} pageout_t;

/*
 * pageout is called by shrink_page_list() for each dirty page.
 * Calls ->writepage().
 */
static pageout_t pageout(struct page *page, struct address_space *mapping)
{
	/*
	 * If the page is dirty, only perform writeback if that write
	 * will be non-blocking.  To prevent this allocation from being
	 * stalled by pagecache activity.  But note that there may be
	 * stalls if we need to run get_block().  We could test
	 * PagePrivate for that.
	 *
	 * If this process is currently in __generic_file_write_iter() against
	 * this page's queue, we can perform writeback even if that
	 * will block.
	 *
	 * If the page is swapcache, write it back even if that would
	 * block, for some throttling. This happens by accident, because
	 * swap_backing_dev_info is bust: it doesn't reflect the
	 * congestion state of the swapdevs.  Easy to fix, if needed.
	 */
	if (!is_page_cache_freeable(page))
		return PAGE_KEEP;
	if (!mapping) {
		/*
		 * Some data journaling orphaned pages can have
		 * page->mapping == NULL while being dirty with clean buffers.
		 */
		if (page_has_private(page)) {
			if (try_to_free_buffers(page)) {
				ClearPageDirty(page);
				pr_info("%s: orphaned page\n", __func__);
				return PAGE_CLEAN;
			}
		}
		return PAGE_KEEP;
	}
	if (mapping->a_ops->writepage == NULL)
		return PAGE_ACTIVATE;
	if (!may_write_to_inode(mapping->host))
		return PAGE_KEEP;

	if (clear_page_dirty_for_io(page)) {
		int res;
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_NONE,
			.nr_to_write = SWAP_CLUSTER_MAX,
			.range_start = 0,
			.range_end = LLONG_MAX,
			.for_reclaim = 1,
		};

		SetPageReclaim(page);
		res = mapping->a_ops->writepage(page, &wbc);
		if (res < 0)
			handle_write_error(mapping, page, res);
		if (res == AOP_WRITEPAGE_ACTIVATE) {
			ClearPageReclaim(page);
			return PAGE_ACTIVATE;
		}

		if (!PageWriteback(page)) {
			/* synchronous write or broken a_ops? */
			ClearPageReclaim(page);
		}
		trace_mm_vmscan_writepage(page, trace_reclaim_flags(page));
		inc_zone_page_state(page, NR_VMSCAN_WRITE);
		return PAGE_SUCCESS;
	}

	return PAGE_CLEAN;
}

/*
 * Same as remove_mapping, but if the page is removed from the mapping, it
 * gets returned with a refcount of 0.
 */
static int __remove_mapping(struct address_space *mapping, struct page *page,
			    bool reclaimed)
{
	unsigned long flags;
	struct mem_cgroup *memcg;

	BUG_ON(!PageLocked(page));
	BUG_ON(mapping != page_mapping(page));

	memcg = mem_cgroup_begin_page_stat(page);
	spin_lock_irqsave(&mapping->tree_lock, flags);
	/*
	 * The non racy check for a busy page.
	 *
	 * Must be careful with the order of the tests. When someone has
	 * a ref to the page, it may be possible that they dirty it then
	 * drop the reference. So if PageDirty is tested before page_count
	 * here, then the following race may occur:
	 *
	 * get_user_pages(&page);
	 * [user mapping goes away]
	 * write_to(page);
	 *				!PageDirty(page)    [good]
	 * SetPageDirty(page);
	 * put_page(page);
	 *				!page_count(page)   [good, discard it]
	 *
	 * [oops, our write_to data is lost]
	 *
	 * Reversing the order of the tests ensures such a situation cannot
	 * escape unnoticed. The smp_rmb is needed to ensure the page->flags
	 * load is not satisfied before that of page->_count.
	 *
	 * Note that if SetPageDirty is always performed via set_page_dirty,
	 * and thus under tree_lock, then this ordering is not required.
	 */
	if (!page_freeze_refs(page, 2))
		goto cannot_free;
	/* note: atomic_cmpxchg in page_freeze_refs provides the smp_rmb */
	if (unlikely(PageDirty(page))) {
		page_unfreeze_refs(page, 2);
		goto cannot_free;
	}

	if (PageSwapCache(page)) {
		swp_entry_t swap = { .val = page_private(page) };
		mem_cgroup_swapout(page, swap);
		__delete_from_swap_cache(page);
		spin_unlock_irqrestore(&mapping->tree_lock, flags);
		mem_cgroup_end_page_stat(memcg);
		swapcache_free(swap);
	} else {
		void (*freepage)(struct page *);
		void *shadow = NULL;

		freepage = mapping->a_ops->freepage;
		/*
		 * Remember a shadow entry for reclaimed file cache in
		 * order to detect refaults, thus thrashing, later on.
		 *
		 * But don't store shadows in an address space that is
		 * already exiting.  This is not just an optizimation,
		 * inode reclaim needs to empty out the radix tree or
		 * the nodes are lost.  Don't plant shadows behind its
		 * back.
		 */
		if (reclaimed && page_is_file_cache(page) &&
		    !mapping_exiting(mapping))
			shadow = workingset_eviction(mapping, page);
		__delete_from_page_cache(page, shadow, memcg);
		spin_unlock_irqrestore(&mapping->tree_lock, flags);
		mem_cgroup_end_page_stat(memcg);

		if (freepage != NULL)
			freepage(page);
	}

	return 1;

cannot_free:
	spin_unlock_irqrestore(&mapping->tree_lock, flags);
	mem_cgroup_end_page_stat(memcg);
	return 0;
}

/*
 * Attempt to detach a locked page from its ->mapping.  If it is dirty or if
 * someone else has a ref on the page, abort and return 0.  If it was
 * successfully detached, return 1.  Assumes the caller has a single ref on
 * this page.
 */
int remove_mapping(struct address_space *mapping, struct page *page)
{
	if (__remove_mapping(mapping, page, false)) {
		/*
		 * Unfreezing the refcount with 1 rather than 2 effectively
		 * drops the pagecache ref for us without requiring another
		 * atomic operation.
		 */
		page_unfreeze_refs(page, 1);
		return 1;
	}
	return 0;
}

/**
 * putback_lru_page - put previously isolated page onto appropriate LRU list
 * @page: page to be put back to appropriate lru list
 *
 * Add previously isolated @page to appropriate LRU list.
 * Page may still be unevictable for other reasons.
 *
 * lru_lock must not be held, interrupts must be enabled.
 */
void putback_lru_page(struct page *page)
{
	bool is_unevictable;
	int was_unevictable = PageUnevictable(page);

	VM_BUG_ON_PAGE(PageLRU(page), page);

redo:
	ClearPageUnevictable(page);

	if (page_evictable(page)) {
		/*
		 * For evictable pages, we can use the cache.
		 * In event of a race, worst case is we end up with an
		 * unevictable page on [in]active list.
		 * We know how to handle that.
		 */
		is_unevictable = false;
		lru_cache_add(page);
	} else {
		/*
		 * Put unevictable pages directly on zone's unevictable
		 * list.
		 */
		is_unevictable = true;
		add_page_to_unevictable_list(page);
		/*
		 * When racing with an mlock or AS_UNEVICTABLE clearing
		 * (page is unlocked) make sure that if the other thread
		 * does not observe our setting of PG_lru and fails
		 * isolation/check_move_unevictable_pages,
		 * we see PG_mlocked/AS_UNEVICTABLE cleared below and move
		 * the page back to the evictable list.
		 *
		 * The other side is TestClearPageMlocked() or shmem_lock().
		 */
		smp_mb();
	}

	/*
	 * page's status can change while we move it among lru. If an evictable
	 * page is on unevictable list, it never be freed. To avoid that,
	 * check after we added it to the list, again.
	 */
	if (is_unevictable && page_evictable(page)) {
		if (!isolate_lru_page(page)) {
			put_page(page);
			goto redo;
		}
		/* This means someone else dropped this page from LRU
		 * So, it will be freed or putback to LRU again. There is
		 * nothing to do here.
		 */
	}

	if (was_unevictable && !is_unevictable)
		count_vm_event(UNEVICTABLE_PGRESCUED);
	else if (!was_unevictable && is_unevictable)
		count_vm_event(UNEVICTABLE_PGCULLED);

	put_page(page);		/* drop ref from isolate */
}

enum page_references {
	PAGEREF_RECLAIM,
	PAGEREF_RECLAIM_CLEAN,
	PAGEREF_KEEP,
	PAGEREF_ACTIVATE,
};

static enum page_references page_check_references(struct page *page,
						  struct scan_control *sc)
{
	int referenced_ptes, referenced_page;
	unsigned long vm_flags;

	referenced_ptes = page_referenced(page, 1, sc->target_mem_cgroup,
					  &vm_flags);
	referenced_page = TestClearPageReferenced(page);

	/*
	 * Mlock lost the isolation race with us.  Let try_to_unmap()
	 * move the page to the unevictable list.
	 */
	if (vm_flags & VM_LOCKED)
		return PAGEREF_RECLAIM;

	if (referenced_ptes) {
		if (PageSwapBacked(page))
			return PAGEREF_ACTIVATE;
		/*
		 * All mapped pages start out with page table
		 * references from the instantiating fault, so we need
		 * to look twice if a mapped file page is used more
		 * than once.
		 *
		 * Mark it and spare it for another trip around the
		 * inactive list.  Another page table reference will
		 * lead to its activation.
		 *
		 * Note: the mark is set for activated pages as well
		 * so that recently deactivated but used pages are
		 * quickly recovered.
		 */
		SetPageReferenced(page);

		if (referenced_page || referenced_ptes > 1)
			return PAGEREF_ACTIVATE;

		/*
		 * Activate file-backed executable pages after first usage.
		 */
		if (vm_flags & VM_EXEC)
			return PAGEREF_ACTIVATE;

		return PAGEREF_KEEP;
	}

	/* Reclaim if clean, defer dirty pages to writeback */
	if (referenced_page && !PageSwapBacked(page))
		return PAGEREF_RECLAIM_CLEAN;

	return PAGEREF_RECLAIM;
}

/* Check if a page is dirty or under writeback */
static void page_check_dirty_writeback(struct page *page,
				       bool *dirty, bool *writeback)
{
	struct address_space *mapping;

	/*
	 * Anonymous pages are not handled by flushers and must be written
	 * from reclaim context. Do not stall reclaim based on them
	 */
	if (!page_is_file_cache(page) ||
	    (PageAnon(page) && !PageSwapBacked(page))) {
		*dirty = false;
		*writeback = false;
		return;
	}

	/* By default assume that the page flags are accurate */
	*dirty = PageDirty(page);
	*writeback = PageWriteback(page);

	/* Verify dirty/writeback state if the filesystem supports it */
	if (!page_has_private(page))
		return;

	mapping = page_mapping(page);
	if (mapping && mapping->a_ops->is_dirty_writeback)
		mapping->a_ops->is_dirty_writeback(page, dirty, writeback);
}

/*
 * shrink_page_list() returns the number of reclaimed pages
 */
static unsigned long shrink_page_list(struct list_head *page_list,
				      struct zone *zone,
				      struct scan_control *sc,
				      enum ttu_flags ttu_flags,
				      unsigned long *ret_nr_dirty,
				      unsigned long *ret_nr_unqueued_dirty,
				      unsigned long *ret_nr_congested,
				      unsigned long *ret_nr_writeback,
				      unsigned long *ret_nr_immediate,
				      bool ignore_references)
{
	LIST_HEAD(ret_pages);
	LIST_HEAD(free_pages);
	int pgactivate = 0;
	unsigned long nr_unqueued_dirty = 0;
	unsigned long nr_dirty = 0;
	unsigned long nr_congested = 0;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_writeback = 0;
	unsigned long nr_immediate = 0;

	cond_resched();

	while (!list_empty(page_list)) {
		struct address_space *mapping;
		struct page *page;
		int may_enter_fs;
		enum page_references references = PAGEREF_RECLAIM;
		bool dirty, writeback;

		cond_resched();

		page = lru_to_page(page_list);
		list_del(&page->lru);

		if (!trylock_page(page))
			goto keep;

		VM_BUG_ON_PAGE(PageActive(page), page);
		VM_BUG_ON_PAGE(page_zone(page) != zone, page);

		sc->nr_scanned++;

		if (unlikely(!page_evictable(page)))
			goto cull_mlocked;

		if (!sc->may_unmap && page_mapped(page))
			goto keep_locked;

		/* page_update_gen() tried to promote this page? */
		if (lru_gen_enabled() && !ignore_references &&
		    page_mapped(page) && PageReferenced(page))
			goto keep_locked;

		/* Double the slab pressure for mapped and swapcache pages */
		if ((page_mapped(page) || PageSwapCache(page)) &&
		    !(PageAnon(page) && !PageSwapBacked(page)))
			sc->nr_scanned++;

		may_enter_fs = (sc->gfp_mask & __GFP_FS) ||
			(PageSwapCache(page) && (sc->gfp_mask & __GFP_IO));

		/*
		 * The number of dirty pages determines if a zone is marked
		 * reclaim_congested which affects wait_iff_congested. kswapd
		 * will stall and start writing pages if the tail of the LRU
		 * is all dirty unqueued pages.
		 */
		page_check_dirty_writeback(page, &dirty, &writeback);
		if (dirty || writeback)
			nr_dirty++;

		if (dirty && !writeback)
			nr_unqueued_dirty++;

		/*
		 * Treat this page as congested if the underlying BDI is or if
		 * pages are cycling through the LRU so quickly that the
		 * pages marked for immediate reclaim are making it to the
		 * end of the LRU a second time.
		 */
		mapping = page_mapping(page);
		if (((dirty || writeback) && mapping &&
		     inode_write_congested(mapping->host)) ||
		    (writeback && PageReclaim(page)))
			nr_congested++;

		/*
		 * If a page at the tail of the LRU is under writeback, there
		 * are three cases to consider.
		 *
		 * 1) If reclaim is encountering an excessive number of pages
		 *    under writeback and this page is both under writeback and
		 *    PageReclaim then it indicates that pages are being queued
		 *    for IO but are being recycled through the LRU before the
		 *    IO can complete. Waiting on the page itself risks an
		 *    indefinite stall if it is impossible to writeback the
		 *    page due to IO error or disconnected storage so instead
		 *    note that the LRU is being scanned too quickly and the
		 *    caller can stall after page list has been processed.
		 *
		 * 2) Global or new memcg reclaim encounters a page that is
		 *    not marked for immediate reclaim, or the caller does not
		 *    have __GFP_FS (or __GFP_IO if it's simply going to swap,
		 *    not to fs). In this case mark the page for immediate
		 *    reclaim and continue scanning.
		 *
		 *    Require may_enter_fs because we would wait on fs, which
		 *    may not have submitted IO yet. And the loop driver might
		 *    enter reclaim, and deadlock if it waits on a page for
		 *    which it is needed to do the write (loop masks off
		 *    __GFP_IO|__GFP_FS for this reason); but more thought
		 *    would probably show more reasons.
		 *
		 * 3) Legacy memcg encounters a page that is already marked
		 *    PageReclaim. memcg does not have any dirty pages
		 *    throttling so we could easily OOM just because too many
		 *    pages are in writeback and there is nothing else to
		 *    reclaim. Wait for the writeback to complete.
		 */
		if (PageWriteback(page)) {
			/* Case 1 above */
			if (current_is_kswapd() &&
			    PageReclaim(page) &&
			    (zone &&
			    test_bit(ZONE_WRITEBACK, &zone->flags))) {
				nr_immediate++;
				goto keep_locked;

			/* Case 2 above */
			} else if (sane_reclaim(sc) ||
			    !PageReclaim(page) || !may_enter_fs) {
				/*
				 * This is slightly racy - end_page_writeback()
				 * might have just cleared PageReclaim, then
				 * setting PageReclaim here end up interpreted
				 * as PageReadahead - but that does not matter
				 * enough to care.  What we do want is for this
				 * page to have PageReclaim set next time memcg
				 * reclaim reaches the tests above, so it will
				 * then wait_on_page_writeback() to avoid OOM;
				 * and it's also appropriate in global reclaim.
				 */
				SetPageReclaim(page);
				nr_writeback++;
				goto keep_locked;

			/* Case 3 above */
			} else {
				unlock_page(page);
				wait_on_page_writeback(page);
				/* then go back and try same page again */
				list_add_tail(&page->lru, page_list);
				continue;
			}
		}

		if (!ignore_references)
			references = page_check_references(page, sc);

		switch (references) {
		case PAGEREF_ACTIVATE:
			goto activate_locked;
		case PAGEREF_KEEP:
			goto keep_locked;
		case PAGEREF_RECLAIM:
		case PAGEREF_RECLAIM_CLEAN:
			; /* try to reclaim the page below */
		}

		/*
		 * Anonymous process memory has backing store?
		 * Try to allocate it some swap space here.
		 * Lazyfree page could be freed directly
		 */
		if (PageAnon(page) && PageSwapBacked(page)) {
			if (!PageSwapCache(page)) {
				if (!(sc->gfp_mask & __GFP_IO))
					goto keep_locked;
				if (!add_to_swap(page, page_list))
					goto activate_locked;
				may_enter_fs = 1;

				/* Adding to swap updated mapping */
				mapping = page_mapping(page);
			}
		}

		/*
		 * The page is mapped into the page tables of one or more
		 * processes. Try to unmap it here.
		 */
		if (page_mapped(page)) {
			switch (try_to_unmap(page,
					ttu_flags|TTU_BATCH_FLUSH, sc->target_vma)) {
			case SWAP_FAIL:
				goto activate_locked;
			case SWAP_AGAIN:
				goto keep_locked;
			case SWAP_MLOCK:
				goto cull_mlocked;
			case SWAP_SUCCESS:
				; /* try to free the page below */
			}
		}

		if (PageDirty(page)) {
			/*
			 * Only kswapd can writeback filesystem pages to
			 * avoid risk of stack overflow but only writeback
			 * if many dirty pages have been encountered.
			 */
			if (page_is_file_cache(page) &&
					(!current_is_kswapd() ||
					 (zone &&
					 !test_bit(ZONE_DIRTY, &zone->flags)))) {
				/*
				 * Immediately reclaim when written back.
				 * Similar in principal to deactivate_page()
				 * except we already have the page isolated
				 * and know it's dirty
				 */
				inc_zone_page_state(page, NR_VMSCAN_IMMEDIATE);
				SetPageReclaim(page);

				goto keep_locked;
			}

			if (references == PAGEREF_RECLAIM_CLEAN)
				goto keep_locked;
			if (!may_enter_fs)
				goto keep_locked;
			if (!sc->may_writepage)
				goto keep_locked;

			/*
			 * Page is dirty. Flush the TLB if a writable entry
			 * potentially exists to avoid CPU writes after IO
			 * starts and then write it out here.
			 */
			try_to_unmap_flush_dirty();
			switch (pageout(page, mapping)) {
			case PAGE_KEEP:
				goto keep_locked;
			case PAGE_ACTIVATE:
				goto activate_locked;
			case PAGE_SUCCESS:
				if (PageWriteback(page))
					goto keep;
				if (PageDirty(page))
					goto keep;

				/*
				 * A synchronous write - probably a ramdisk.  Go
				 * ahead and try to reclaim the page.
				 */
				if (!trylock_page(page))
					goto keep;
				if (PageDirty(page) || PageWriteback(page))
					goto keep_locked;
				mapping = page_mapping(page);
			case PAGE_CLEAN:
				; /* try to free the page below */
			}
		}

		/*
		 * If the page has buffers, try to free the buffer mappings
		 * associated with this page. If we succeed we try to free
		 * the page as well.
		 *
		 * We do this even if the page is PageDirty().
		 * try_to_release_page() does not perform I/O, but it is
		 * possible for a page to have PageDirty set, but it is actually
		 * clean (all its buffers are clean).  This happens if the
		 * buffers were written out directly, with submit_bh(). ext3
		 * will do this, as well as the blockdev mapping.
		 * try_to_release_page() will discover that cleanness and will
		 * drop the buffers and mark the page clean - it can be freed.
		 *
		 * Rarely, pages can have buffers and no ->mapping.  These are
		 * the pages which were not successfully invalidated in
		 * truncate_complete_page().  We try to drop those buffers here
		 * and if that worked, and the page is no longer mapped into
		 * process address space (page_count == 1) it can be freed.
		 * Otherwise, leave the page on the LRU so it is swappable.
		 */
		if (page_has_private(page)) {
			if (!try_to_release_page(page, sc->gfp_mask))
				goto activate_locked;
			if (!mapping && page_count(page) == 1) {
				unlock_page(page);
				if (put_page_testzero(page))
					goto free_it;
				else {
					/*
					 * rare race with speculative reference.
					 * the speculative reference will free
					 * this page shortly, so we may
					 * increment nr_reclaimed here (and
					 * leave it off the LRU).
					 */
					nr_reclaimed++;
					continue;
				}
			}
		}

		if (PageAnon(page) && !PageSwapBacked(page)) {
			/* follow __remove_mapping for reference */
			if (!page_freeze_refs(page, 1))
				goto keep_locked;
			if (PageDirty(page)) {
				page_unfreeze_refs(page, 1);
				goto keep_locked;
			}

			count_vm_event(PGLAZYFREED);
		} else if (!mapping || !__remove_mapping(mapping, page, true))
			goto keep_locked;

		/*
		 * At this point, we have no other references and there is
		 * no way to pick any more up (removed from LRU, removed
		 * from pagecache). Can use non-atomic bitops now (and
		 * we obviously don't have to worry about waking up a process
		 * waiting on the page lock, because there are no references.
		 */
		__clear_page_locked(page);
free_it:
		nr_reclaimed++;

		/*
		 * Is there need to periodically free_page_list? It would
		 * appear not as the counts should be low
		 */
		list_add(&page->lru, &free_pages);
		/*
		 * If pagelist are from multiple nodes, we should decrease
		 * NR_ISOLATED_ANON + x on freed pages in here.
		 */
		if (!zone)
			dec_zone_page_state(page, NR_ISOLATED_ANON +
					page_is_file_cache(page));
		continue;

cull_mlocked:
		if (PageSwapCache(page))
			try_to_free_swap(page);
		unlock_page(page);
		list_add(&page->lru, &ret_pages);
		continue;

activate_locked:
		/* Not a candidate for swapping, so reclaim swap space. */
		if (PageSwapCache(page) && vm_swap_full())
			try_to_free_swap(page);
		VM_BUG_ON_PAGE(PageActive(page), page);
		SetPageActive(page);
		pgactivate++;
keep_locked:
		unlock_page(page);
keep:
		list_add(&page->lru, &ret_pages);
		VM_BUG_ON_PAGE(PageLRU(page) || PageUnevictable(page), page);
	}

	mem_cgroup_uncharge_list(&free_pages);
	try_to_unmap_flush();
	free_hot_cold_page_list(&free_pages, true);

	list_splice(&ret_pages, page_list);
	count_vm_events(PGACTIVATE, pgactivate);

	*ret_nr_dirty += nr_dirty;
	*ret_nr_congested += nr_congested;
	*ret_nr_unqueued_dirty += nr_unqueued_dirty;
	*ret_nr_writeback += nr_writeback;
	*ret_nr_immediate += nr_immediate;
	return nr_reclaimed;
}

unsigned long reclaim_clean_pages_from_list(struct zone *zone,
					    struct list_head *page_list)
{
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.priority = DEF_PRIORITY,
		.may_unmap = 1,
		/* Doesn't allow to write out dirty page */
		.may_writepage = 0,
	};
	unsigned long ret, dummy1, dummy2, dummy3, dummy4, dummy5;
	struct page *page, *next;
	LIST_HEAD(clean_pages);

	list_for_each_entry_safe(page, next, page_list, lru) {
		if (page_is_file_cache(page) && !PageDirty(page) &&
		    !__PageMovable(page)) {
			ClearPageActive(page);
			list_move(&page->lru, &clean_pages);
		}
	}

	ret = shrink_page_list(&clean_pages, zone, &sc,
			TTU_UNMAP|TTU_IGNORE_ACCESS,
			&dummy1, &dummy2, &dummy3, &dummy4, &dummy5, true);
	list_splice(&clean_pages, page_list);
	mod_zone_page_state(zone, NR_ISOLATED_FILE, -ret);
	return ret;
}

#ifdef CONFIG_PROCESS_RECLAIM
unsigned long reclaim_pages_from_list(struct list_head *page_list,
					struct vm_area_struct *vma)
{
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.priority = DEF_PRIORITY,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.target_vma = vma,
	};

	unsigned long nr_reclaimed;
	unsigned long nr_dirty = 0;
	unsigned long nr_congested = 0;
	unsigned long nr_unqueued_dirty = 0;
	unsigned long nr_writeback = 0;
	unsigned long nr_immediate = 0;
	struct page *page, *next;
	LIST_HEAD(unevictable_pages);

	list_for_each_entry_safe(page, next, page_list, lru) {
		if (PageUnevictable(page)) {
			list_move(&page->lru, &unevictable_pages);
			continue;
		}
		ClearPageActive(page);
	}

	nr_reclaimed = shrink_page_list(page_list, NULL, &sc,
			TTU_IGNORE_ACCESS, &nr_dirty, &nr_unqueued_dirty, &nr_congested,
				&nr_writeback, &nr_immediate, true);

	list_splice(&unevictable_pages, page_list);
	while (!list_empty(page_list)) {
		page = lru_to_page(page_list);
		list_del(&page->lru);
		dec_zone_page_state(page, NR_ISOLATED_ANON +
				page_is_file_cache(page));
		putback_lru_page(page);
	}

	return nr_reclaimed;
}
#endif

/*
 * Attempt to remove the specified page from its LRU.  Only take this page
 * if it is of the appropriate PageActive status.  Pages which are being
 * freed elsewhere are also ignored.
 *
 * page:	page to consider
 * mode:	one of the LRU isolation modes defined above
 *
 * returns 0 on success, -ve errno on failure.
 */
int __isolate_lru_page(struct page *page, isolate_mode_t mode)
{
	int ret = -EINVAL;

	/* Only take pages on the LRU. */
	if (!PageLRU(page))
		return ret;

	/* Compaction should not handle unevictable pages but CMA can do so */
	if (PageUnevictable(page) && !(mode & ISOLATE_UNEVICTABLE))
		return ret;

	ret = -EBUSY;

	/*
	 * To minimise LRU disruption, the caller can indicate that it only
	 * wants to isolate pages it will be able to operate on without
	 * blocking - clean pages for the most part.
	 *
	 * ISOLATE_CLEAN means that only clean pages should be isolated. This
	 * is used by reclaim when it is cannot write to backing storage
	 *
	 * ISOLATE_ASYNC_MIGRATE is used to indicate that it only wants to pages
	 * that it is possible to migrate without blocking
	 */
	if (mode & (ISOLATE_CLEAN|ISOLATE_ASYNC_MIGRATE)) {
		/* All the caller can do on PageWriteback is block */
		if (PageWriteback(page))
			return ret;

		if (PageDirty(page)) {
			struct address_space *mapping;
			bool migrate_dirty;

			/* ISOLATE_CLEAN means only clean pages */
			if (mode & ISOLATE_CLEAN)
				return ret;

			/*
			 * Only pages without mappings or that have a
			 * ->migratepage callback are possible to migrate
			 * without blocking. However, we can be racing with
			 * truncation so it's necessary to lock the page
			 * to stabilise the mapping as truncation holds
			 * the page lock until after the page is removed
			 * from the page cache.
			 */
			if (!trylock_page(page))
				return ret;

			mapping = page_mapping(page);
			migrate_dirty = !mapping || mapping->a_ops->migratepage;
			unlock_page(page);
			if (!migrate_dirty)
				return ret;
		}
	}

	if ((mode & ISOLATE_UNMAPPED) && page_mapped(page))
		return ret;

	if (likely(get_page_unless_zero(page))) {
		/*
		 * Be careful not to clear PageLRU until after we're
		 * sure the page is not being freed elsewhere -- the
		 * page release code relies on it.
		 */
		ClearPageLRU(page);
		ret = 0;
	}

	return ret;
}

/*
 * zone->lru_lock is heavily contended.  Some of the functions that
 * shrink the lists perform better by taking out a batch of pages
 * and working on them outside the LRU lock.
 *
 * For pagecache intensive workloads, this function is the hottest
 * spot in the kernel (apart from copy_*_user functions).
 *
 * Appropriate locks must be held before calling this function.
 *
 * @nr_to_scan:	The number of pages to look through on the list.
 * @lruvec:	The LRU vector to pull pages from.
 * @dst:	The temp list to put pages on to.
 * @nr_scanned:	The number of pages that were scanned.
 * @sc:		The scan_control struct for this reclaim session
 * @mode:	One of the LRU isolation modes
 * @lru:	LRU list id for isolating
 *
 * returns how many pages were moved onto *@dst.
 */
static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned, struct scan_control *sc,
		isolate_mode_t mode, enum lru_list lru)
{
	struct list_head *src = &lruvec->lists[lru];
	unsigned long nr_taken = 0;
	unsigned long scan;

	for (scan = 0; scan < nr_to_scan && nr_taken < nr_to_scan &&
					!list_empty(src); scan++) {
		struct page *page;
		int nr_pages;

		page = lru_to_page(src);
		prefetchw_prev_lru_page(page, src, flags);

		VM_BUG_ON_PAGE(!PageLRU(page), page);

		switch (__isolate_lru_page(page, mode)) {
		case 0:
			nr_pages = hpage_nr_pages(page);
			mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
			list_move(&page->lru, dst);
			nr_taken += nr_pages;
			break;

		case -EBUSY:
			/* else it is being freed elsewhere */
			list_move(&page->lru, src);
			continue;

		default:
			BUG();
		}
	}

	*nr_scanned = scan;
	trace_mm_vmscan_lru_isolate(sc->order, nr_to_scan, scan,
				    nr_taken, mode, is_file_lru(lru));
	return nr_taken;
}

/**
 * isolate_lru_page - tries to isolate a page from its LRU list
 * @page: page to isolate from its LRU list
 *
 * Isolates a @page from an LRU list, clears PageLRU and adjusts the
 * vmstat statistic corresponding to whatever LRU list the page was on.
 *
 * Returns 0 if the page was removed from an LRU list.
 * Returns -EBUSY if the page was not on an LRU list.
 *
 * The returned page will have PageLRU() cleared.  If it was found on
 * the active list, it will have PageActive set.  If it was found on
 * the unevictable list, it will have the PageUnevictable bit set. That flag
 * may need to be cleared by the caller before letting the page go.
 *
 * The vmstat statistic corresponding to the list on which the page was
 * found will be decremented.
 *
 * Restrictions:
 * (1) Must be called with an elevated refcount on the page. This is a
 *     fundamentnal difference from isolate_lru_pages (which is called
 *     without a stable reference).
 * (2) the lru_lock must not be held.
 * (3) interrupts must be enabled.
 */
int isolate_lru_page(struct page *page)
{
	int ret = -EBUSY;

	VM_BUG_ON_PAGE(!page_count(page), page);

	if (PageLRU(page)) {
		struct zone *zone = page_zone(page);
		struct lruvec *lruvec;

		spin_lock_irq(&zone->lru_lock);
		lruvec = mem_cgroup_page_lruvec(page, zone);
		if (PageLRU(page)) {
			int lru = page_lru(page);
			get_page(page);
			ClearPageLRU(page);
			del_page_from_lru_list(page, lruvec, lru);
			ret = 0;
		}
		spin_unlock_irq(&zone->lru_lock);
	}
	return ret;
}

/*
 * A direct reclaimer may isolate SWAP_CLUSTER_MAX pages from the LRU list and
 * then get resheduled. When there are massive number of tasks doing page
 * allocation, such sleeping direct reclaimers may keep piling up on each CPU,
 * the LRU list will go small and be scanned faster than necessary, leading to
 * unnecessary swapping, thrashing and OOM.
 */
static int too_many_isolated(struct zone *zone, int file,
		struct scan_control *sc)
{
	unsigned long inactive, isolated;

	if (current_is_kswapd())
		return 0;

	if (!sane_reclaim(sc))
		return 0;

	if (file) {
		inactive = zone_page_state(zone, NR_INACTIVE_FILE);
		isolated = zone_page_state(zone, NR_ISOLATED_FILE);
	} else {
		inactive = zone_page_state(zone, NR_INACTIVE_ANON);
		isolated = zone_page_state(zone, NR_ISOLATED_ANON);
	}

	/*
	 * GFP_NOIO/GFP_NOFS callers are allowed to isolate more pages, so they
	 * won't get blocked by normal direct-reclaimers, forming a circular
	 * deadlock.
	 */
	if ((sc->gfp_mask & (__GFP_IO | __GFP_FS)) == (__GFP_IO | __GFP_FS))
		inactive >>= 3;

	return isolated > inactive;
}

static noinline_for_stack void
putback_inactive_pages(struct lruvec *lruvec, struct list_head *page_list)
{
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	struct zone *zone = lruvec_zone(lruvec);
	LIST_HEAD(pages_to_free);

	/*
	 * Put back any unfreeable pages.
	 */
	while (!list_empty(page_list)) {
		struct page *page = lru_to_page(page_list);
		int lru;

		VM_BUG_ON_PAGE(PageLRU(page), page);
		list_del(&page->lru);
		if (unlikely(!page_evictable(page))) {
			spin_unlock_irq(&zone->lru_lock);
			putback_lru_page(page);
			spin_lock_irq(&zone->lru_lock);
			continue;
		}

		lruvec = mem_cgroup_page_lruvec(page, zone);

		SetPageLRU(page);

		/*
		 * The page may have lost its last reference while isolated (e.g.,
		 * a parallel release_pages()). It must be freed without going back
		 * on an LRU list: lru_gen_del_page() re-stamps the PG_active marker
		 * when a page is deleted from an active generation, and a page that
		 * is freed instead of migrated must not carry it into
		 * free_pages_prepare() (see the comment on MIN_NR_GENS about
		 * PG_active). Same shape as the 5.15 reference move_pages_to_lru().
		 */
		if (unlikely(put_page_testzero(page))) {
			__ClearPageLRU(page);
			__ClearPageActive(page);

			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&zone->lru_lock);
				mem_cgroup_uncharge(page);
				(*get_compound_page_dtor(page))(page);
				spin_lock_irq(&zone->lru_lock);
			} else
				list_add(&page->lru, &pages_to_free);
			continue;
		}

		lru = page_lru(page);
		add_page_to_lru_list(page, lruvec, lru);

		if (is_active_lru(lru)) {
			int file = is_file_lru(lru);
			int numpages = hpage_nr_pages(page);
			reclaim_stat->recent_rotated[file] += numpages;
		}
	}

	/*
	 * To save our caller's stack, now use input list for pages to free.
	 */
	list_splice(&pages_to_free, page_list);
}

/*
 * If a kernel thread (such as nfsd for loop-back mounts) services
 * a backing device by writing to the page cache it sets PF_LESS_THROTTLE.
 * In that case we should only throttle if the backing device it is
 * writing to is congested.  In other cases it is safe to throttle.
 */
static int current_may_throttle(void)
{
	return !(current->flags & PF_LESS_THROTTLE) ||
		current->backing_dev_info == NULL ||
		bdi_write_congested(current->backing_dev_info);
}

/*
 * shrink_inactive_list() is a helper for shrink_zone().  It returns the number
 * of reclaimed pages
 */
static noinline_for_stack unsigned long
shrink_inactive_list(unsigned long nr_to_scan, struct lruvec *lruvec,
		     struct scan_control *sc, enum lru_list lru)
{
	LIST_HEAD(page_list);
	unsigned long nr_scanned;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_taken;
	unsigned long nr_dirty = 0;
	unsigned long nr_congested = 0;
	unsigned long nr_unqueued_dirty = 0;
	unsigned long nr_writeback = 0;
	unsigned long nr_immediate = 0;
	isolate_mode_t isolate_mode = 0;
	int file = is_file_lru(lru);
	struct zone *zone = lruvec_zone(lruvec);
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	bool ignore_references = false;
	enum ttu_flags ttu = TTU_UNMAP;

	while (unlikely(too_many_isolated(zone, file, sc))) {
		congestion_wait(BLK_RW_ASYNC, HZ/10);

		/* We are about to die and free our memory. Return now. */
		if (fatal_signal_pending(current))
			return SWAP_CLUSTER_MAX;
	}

	lru_add_drain();

	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;

	spin_lock_irq(&zone->lru_lock);

	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &page_list,
				     &nr_scanned, sc, isolate_mode, lru);

	__mod_zone_page_state(zone, NR_LRU_BASE + lru, -nr_taken);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, nr_taken);

	if (global_reclaim(sc)) {
		__mod_zone_page_state(zone, NR_PAGES_SCANNED, nr_scanned);
		if (current_is_kswapd())
			__count_zone_vm_events(PGSCAN_KSWAPD, zone, nr_scanned);
		else
			__count_zone_vm_events(PGSCAN_DIRECT, zone, nr_scanned);
	}
	spin_unlock_irq(&zone->lru_lock);

	if (nr_taken == 0)
		return 0;

	if (need_memory_boosting(zone, false)) {
		ignore_references = true;
		ttu |= TTU_IGNORE_ACCESS;
	}

	nr_reclaimed = shrink_page_list(&page_list, zone, sc, ttu,
				&nr_dirty, &nr_unqueued_dirty, &nr_congested,
				&nr_writeback, &nr_immediate,
				ignore_references);

	spin_lock_irq(&zone->lru_lock);

	reclaim_stat->recent_scanned[file] += nr_taken;

	if (global_reclaim(sc)) {
		if (current_is_kswapd())
			__count_zone_vm_events(PGSTEAL_KSWAPD, zone,
					       nr_reclaimed);
		else
			__count_zone_vm_events(PGSTEAL_DIRECT, zone,
					       nr_reclaimed);
	}

	putback_inactive_pages(lruvec, &page_list);

	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, -nr_taken);

	spin_unlock_irq(&zone->lru_lock);

	mem_cgroup_uncharge_list(&page_list);
	free_hot_cold_page_list(&page_list, true);

	/*
	 * If reclaim is isolating dirty pages under writeback, it implies
	 * that the long-lived page allocation rate is exceeding the page
	 * laundering rate. Either the global limits are not being effective
	 * at throttling processes due to the page distribution throughout
	 * zones or there is heavy usage of a slow backing device. The
	 * only option is to throttle from reclaim context which is not ideal
	 * as there is no guarantee the dirtying process is throttled in the
	 * same way balance_dirty_pages() manages.
	 *
	 * Once a zone is flagged ZONE_WRITEBACK, kswapd will count the number
	 * of pages under pages flagged for immediate reclaim and stall if any
	 * are encountered in the nr_immediate check below.
	 */
	if (nr_writeback && nr_writeback == nr_taken)
		set_bit(ZONE_WRITEBACK, &zone->flags);

	/*
	 * Legacy memcg will stall in page writeback so avoid forcibly
	 * stalling here.
	 */
	if (sane_reclaim(sc)) {
		/*
		 * Tag a zone as congested if all the dirty pages scanned were
		 * backed by a congested BDI and wait_iff_congested will stall.
		 */
		if (nr_dirty && nr_dirty == nr_congested)
			set_bit(ZONE_CONGESTED, &zone->flags);

		/*
		 * If dirty pages are scanned that are not queued for IO, it
		 * implies that flushers are not keeping up. In this case, flag
		 * the zone ZONE_DIRTY and kswapd will start writing pages from
		 * reclaim context.
		 */
		if (nr_unqueued_dirty == nr_taken)
			set_bit(ZONE_DIRTY, &zone->flags);

		/*
		 * If kswapd scans pages marked marked for immediate
		 * reclaim and under writeback (nr_immediate), it implies
		 * that pages are cycling through the LRU faster than
		 * they are written so also forcibly stall.
		 */
		if (nr_immediate && current_may_throttle())
			congestion_wait(BLK_RW_ASYNC, HZ/10);
	}

	/*
	 * Stall direct reclaim for IO completions if underlying BDIs or zone
	 * is congested. Allow kswapd to continue until it starts encountering
	 * unqueued dirty pages or cycling through the LRU too quickly.
	 */
	if (!sc->hibernation_mode && !current_is_kswapd() &&
	    current_may_throttle())
		wait_iff_congested(zone, BLK_RW_ASYNC, HZ/10);

	trace_mm_vmscan_lru_shrink_inactive(zone->zone_pgdat->node_id,
		zone_idx(zone),
		nr_scanned, nr_reclaimed,
		sc->priority,
		trace_shrink_flags(file));
	return nr_reclaimed;
}

/*
 * This moves pages from the active list to the inactive list.
 *
 * We move them the other way if the page is referenced by one or more
 * processes, from rmap.
 *
 * If the pages are mostly unmapped, the processing is fast and it is
 * appropriate to hold zone->lru_lock across the whole operation.  But if
 * the pages are mapped, the processing is slow (page_referenced()) so we
 * should drop zone->lru_lock around each page.  It's impossible to balance
 * this, so instead we remove the pages from the LRU while processing them.
 * It is safe to rely on PG_active against the non-LRU pages in here because
 * nobody will play with that bit on a non-LRU page.
 *
 * The downside is that we have to touch page->_count against each page.
 * But we had to alter page->flags anyway.
 */

static void move_active_pages_to_lru(struct lruvec *lruvec,
				     struct list_head *list,
				     struct list_head *pages_to_free,
				     enum lru_list lru)
{
	struct zone *zone = lruvec_zone(lruvec);
	unsigned long pgmoved = 0;
	struct page *page;
	int nr_pages;

	while (!list_empty(list)) {
		page = lru_to_page(list);
		lruvec = mem_cgroup_page_lruvec(page, zone);

		VM_BUG_ON_PAGE(PageLRU(page), page);
		SetPageLRU(page);

		nr_pages = hpage_nr_pages(page);
		mem_cgroup_update_lru_size(lruvec, lru, nr_pages);
		list_move(&page->lru, &lruvec->lists[lru]);
		pgmoved += nr_pages;

		if (put_page_testzero(page)) {
			__ClearPageLRU(page);
			__ClearPageActive(page);
			del_page_from_lru_list(page, lruvec, lru);

			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&zone->lru_lock);
				mem_cgroup_uncharge(page);
				(*get_compound_page_dtor(page))(page);
				spin_lock_irq(&zone->lru_lock);
			} else
				list_add(&page->lru, pages_to_free);
		}
	}
	__mod_zone_page_state(zone, NR_LRU_BASE + lru, pgmoved);
	if (!is_active_lru(lru))
		__count_vm_events(PGDEACTIVATE, pgmoved);
}

static void shrink_active_list(unsigned long nr_to_scan,
			       struct lruvec *lruvec,
			       struct scan_control *sc,
			       enum lru_list lru)
{
	unsigned long nr_taken;
	unsigned long nr_scanned;
	unsigned long vm_flags;
	LIST_HEAD(l_hold);	/* The pages which were snipped off */
	LIST_HEAD(l_active);
	LIST_HEAD(l_inactive);
	struct page *page;
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	unsigned long nr_rotated = 0;
	isolate_mode_t isolate_mode = 0;
	int file = is_file_lru(lru);
	struct zone *zone = lruvec_zone(lruvec);

	lru_add_drain();

	if (!sc->may_unmap)
		isolate_mode |= ISOLATE_UNMAPPED;
	if (!sc->may_writepage)
		isolate_mode |= ISOLATE_CLEAN;

	spin_lock_irq(&zone->lru_lock);

	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, isolate_mode, lru);
	if (global_reclaim(sc))
		__mod_zone_page_state(zone, NR_PAGES_SCANNED, nr_scanned);

	reclaim_stat->recent_scanned[file] += nr_taken;

	__count_zone_vm_events(PGREFILL, zone, nr_scanned);
	__mod_zone_page_state(zone, NR_LRU_BASE + lru, -nr_taken);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, nr_taken);
	spin_unlock_irq(&zone->lru_lock);

	while (!list_empty(&l_hold)) {
		cond_resched();
		page = lru_to_page(&l_hold);
		list_del(&page->lru);

		if (unlikely(!page_evictable(page))) {
			putback_lru_page(page);
			continue;
		}

		if (unlikely(buffer_heads_over_limit)) {
			if (page_has_private(page) && trylock_page(page)) {
				if (page_has_private(page))
					try_to_release_page(page, 0);
				unlock_page(page);
			}
		}

		if (page_referenced(page, 0, sc->target_mem_cgroup,
				    &vm_flags)) {
			nr_rotated += hpage_nr_pages(page);
			/*
			 * Identify referenced, file-backed active pages and
			 * give them one more trip around the active list. So
			 * that executable code get better chances to stay in
			 * memory under moderate memory pressure.  Anon pages
			 * are not likely to be evicted by use-once streaming
			 * IO, plus JVM can create lots of anon VM_EXEC pages,
			 * so we ignore them here.
			 */
			if ((vm_flags & VM_EXEC) && page_is_file_cache(page)) {
				list_add(&page->lru, &l_active);
				continue;
			}
		}

		ClearPageActive(page);	/* we are de-activating */
		SetPageWorkingset(page);
		list_add(&page->lru, &l_inactive);
	}

	/*
	 * Move pages back to the lru list.
	 */
	spin_lock_irq(&zone->lru_lock);
	/*
	 * Count referenced pages from currently used mappings as rotated,
	 * even though only some of them are actually re-activated.  This
	 * helps balance scan pressure between file and anonymous pages in
	 * get_scan_count.
	 */
	reclaim_stat->recent_rotated[file] += nr_rotated;

	move_active_pages_to_lru(lruvec, &l_active, &l_hold, lru);
	move_active_pages_to_lru(lruvec, &l_inactive, &l_hold, lru - LRU_ACTIVE);
	__mod_zone_page_state(zone, NR_ISOLATED_ANON + file, -nr_taken);
	spin_unlock_irq(&zone->lru_lock);

	mem_cgroup_uncharge_list(&l_hold);
	free_hot_cold_page_list(&l_hold, true);
}

#ifdef CONFIG_SWAP
static bool inactive_anon_is_low_global(struct zone *zone)
{
	unsigned long active, inactive;

	active = zone_page_state(zone, NR_ACTIVE_ANON);
	inactive = zone_page_state(zone, NR_INACTIVE_ANON);

	return inactive * zone->inactive_ratio < active;
}

/**
 * inactive_anon_is_low - check if anonymous pages need to be deactivated
 * @lruvec: LRU vector to check
 *
 * Returns true if the zone does not have enough inactive anon pages,
 * meaning some active anon pages need to be deactivated.
 */
static bool inactive_anon_is_low(struct lruvec *lruvec)
{
	/*
	 * If we don't have swap space, anonymous page deactivation
	 * is pointless.
	 */
	if (!total_swap_pages)
		return false;

	if (!mem_cgroup_disabled())
		return mem_cgroup_inactive_anon_is_low(lruvec);

	return inactive_anon_is_low_global(lruvec_zone(lruvec));
}
#else
static inline bool inactive_anon_is_low(struct lruvec *lruvec)
{
	return false;
}
#endif

/**
 * inactive_file_is_low - check if file pages need to be deactivated
 * @lruvec: LRU vector to check
 *
 * When the system is doing streaming IO, memory pressure here
 * ensures that active file pages get deactivated, until more
 * than half of the file pages are on the inactive list.
 *
 * Once we get to that situation, protect the system's working
 * set from being evicted by disabling active file page aging.
 *
 * This uses a different ratio than the anonymous pages, because
 * the page cache uses a use-once replacement algorithm.
 */
static bool inactive_file_is_low(struct lruvec *lruvec)
{
	unsigned long inactive;
	unsigned long active;

	inactive = get_lru_size(lruvec, LRU_INACTIVE_FILE);
	active = get_lru_size(lruvec, LRU_ACTIVE_FILE);

	return active > inactive;
}

static bool inactive_list_is_low(struct lruvec *lruvec, enum lru_list lru)
{
	if (is_file_lru(lru))
		return inactive_file_is_low(lruvec);
	else
		return inactive_anon_is_low(lruvec);
}

static unsigned long shrink_list(enum lru_list lru, unsigned long nr_to_scan,
				 struct lruvec *lruvec, struct scan_control *sc)
{
	if (is_active_lru(lru)) {
		if (inactive_list_is_low(lruvec, lru))
			shrink_active_list(nr_to_scan, lruvec, sc, lru);
		return 0;
	}

	return shrink_inactive_list(nr_to_scan, lruvec, sc, lru);
}

enum scan_balance {
	SCAN_EQUAL,
	SCAN_FRACT,
	SCAN_ANON,
	SCAN_FILE,
};

/* mem_boost throttles only kswapd's behavior */
enum mem_boost {
	NO_BOOST,
	BOOST_MID = 1,
	BOOST_HIGH = 2,
};
static int mem_boost_mode = NO_BOOST;
static unsigned long last_mode_change;
static bool memory_boosting_disabled = false;
static bool am_app_launch = false;

#define MEM_BOOST_MAX_TIME (5 * HZ) /* 5 sec */

#ifdef CONFIG_SYSFS
static ssize_t mem_boost_mode_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	if (time_after(jiffies, last_mode_change + MEM_BOOST_MAX_TIME))
		mem_boost_mode = NO_BOOST;
	return sprintf(buf, "%d\n", mem_boost_mode);
}

static ssize_t mem_boost_mode_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int mode;
	int err;

	err = kstrtoint(buf, 10, &mode);
	if (err || mode > BOOST_HIGH || mode < NO_BOOST)
		return -EINVAL;

	mem_boost_mode = mode;
	last_mode_change = jiffies;

	return count;
}

static ssize_t am_mem_boost_mode_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	if (time_after(jiffies, last_mode_change + MEM_BOOST_MAX_TIME))
		mem_boost_mode = NO_BOOST;
	return sprintf(buf, "%d\n", mem_boost_mode);
}

static ssize_t am_mem_boost_mode_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int mode;
	int err;

	err = kstrtoint(buf, 10, &mode);
	if (err || mode > BOOST_HIGH || mode < NO_BOOST)
		return -EINVAL;

	mem_boost_mode = mode;
	last_mode_change = jiffies;

	return count;
}

static ssize_t disable_mem_boost_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	int ret;

	ret = memory_boosting_disabled ? 1 : 0;
	return sprintf(buf, "%d\n", ret);
}

static ssize_t disable_mem_boost_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int mode;
	int err;

	err = kstrtoint(buf, 10, &mode);
	if (err || (mode != 0 && mode != 1))
		return -EINVAL;

	memory_boosting_disabled = mode ? true : false;

	return count;
}

ATOMIC_NOTIFIER_HEAD(am_app_launch_notifier);

int am_app_launch_notifier_register(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&am_app_launch_notifier, nb);
}

int am_app_launch_notifier_unregister(struct notifier_block *nb)
{
	return  atomic_notifier_chain_unregister(&am_app_launch_notifier, nb);
}

static ssize_t am_app_launch_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	int ret;

	ret = am_app_launch ? 1 : 0;
	return sprintf(buf, "%d\n", ret);
}

static int notify_app_launch_started(void)
{
	pr_debug("am_app_launch started\n");
	atomic_notifier_call_chain(&am_app_launch_notifier, 1, NULL);
	return 0;
}

static int notify_app_launch_finished(void)
{
	pr_debug("am_app_launch finished\n");
	atomic_notifier_call_chain(&am_app_launch_notifier, 0, NULL);
	return 0;
}

static ssize_t am_app_launch_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int mode;
	int err;
	bool am_app_launch_new;

	err = kstrtoint(buf, 10, &mode);
	if (err || (mode != 0 && mode != 1))
		return -EINVAL;

	am_app_launch_new = mode ? true : false;
	pr_debug("am_app_launch %d -> %d\n", am_app_launch,
		     am_app_launch_new);
	if (am_app_launch != am_app_launch_new) {
		if (am_app_launch_new)
			notify_app_launch_started();
		else
			notify_app_launch_finished();
	}
	am_app_launch = am_app_launch_new;

	return count;
}

#define MEM_BOOST_ATTR(_name) \
	static struct kobj_attribute _name##_attr = \
		__ATTR(_name, 0644, _name##_show, _name##_store)
MEM_BOOST_ATTR(mem_boost_mode);
MEM_BOOST_ATTR(am_mem_boost_mode);
MEM_BOOST_ATTR(disable_mem_boost);
MEM_BOOST_ATTR(am_app_launch);

static struct attribute *vmscan_attrs[] = {
	&mem_boost_mode_attr.attr,
	&am_mem_boost_mode_attr.attr,
	&disable_mem_boost_attr.attr,
	&am_app_launch_attr.attr,
	NULL,
};

static struct attribute_group vmscan_attr_group = {
	.attrs = vmscan_attrs,
	.name = "vmscan",
};
#endif

static inline bool mem_boost_pgdat_wmark(struct zone *zone)
{
	return zone_watermark_ok_safe(zone, 0, low_wmark_pages(zone), 0); //TODO: low, high, or (low + high)/2
}

#define MEM_BOOST_THRESHOLD \
	((CONFIG_MEM_BOOST_THRESHOLD_MB * 1024 * 1024) / (PAGE_SIZE))

bool need_memory_boosting(struct zone *zone, bool skip)
{
	bool ret;
	unsigned long pgdatfile = global_page_state(NR_ACTIVE_FILE) +
				global_page_state(NR_INACTIVE_FILE);

	if (time_after(jiffies, last_mode_change + MEM_BOOST_MAX_TIME) || pgdatfile < MEM_BOOST_THRESHOLD)
		mem_boost_mode = NO_BOOST;

	if (!skip && memory_boosting_disabled)
		return false;

	switch (mem_boost_mode) {
	case BOOST_HIGH:
		ret = true;
		break;
	case BOOST_MID:
		ret = mem_boost_pgdat_wmark(zone) ? false : true;
		break;
	case NO_BOOST:
	default:
		ret = false;
		break;
	}
	return ret;
}

/*
 * Determine how aggressively the anon and file LRU lists should be
 * scanned.  The relative value of each set of LRU lists is determined
 * by looking at the fraction of the pages scanned we did rotate back
 * onto the active list instead of evict.
 *
 * nr[0] = anon inactive pages to scan; nr[1] = anon active pages to scan
 * nr[2] = file inactive pages to scan; nr[3] = file active pages to scan
 */
static void get_scan_count(struct lruvec *lruvec, int swappiness,
			   struct scan_control *sc, unsigned long *nr,
			   unsigned long *lru_pages)
{
	struct zone_reclaim_stat *reclaim_stat = &lruvec->reclaim_stat;
	u64 fraction[2];
	u64 denominator = 0;	/* gcc */
	struct zone *zone = lruvec_zone(lruvec);
	unsigned long anon_prio, file_prio;
	enum scan_balance scan_balance;
	unsigned long anon, file;
	bool force_scan = false;
	unsigned long ap, fp;
	enum lru_list lru;
	bool some_scanned;
	int pass;

	/*
	 * If the zone or memcg is small, nr[l] can be 0.  This
	 * results in no scanning on this priority and a potential
	 * priority drop.  Global direct reclaim can go to the next
	 * zone and tends to have no problems. Global kswapd is for
	 * zone balancing and it needs to scan a minimum amount. When
	 * reclaiming for a memcg, a priority drop can cause high
	 * latencies, so it's better to scan a minimum amount there as
	 * well.
	 */
	if (current_is_kswapd()) {
		if (!zone_reclaimable(zone))
			force_scan = true;
		if (!mem_cgroup_lruvec_online(lruvec))
			force_scan = true;
	}
	if (!global_reclaim(sc))
		force_scan = true;

	/* If we have no swap space, do not bother scanning anon pages. */
	if (!sc->may_swap || (get_nr_swap_pages() <= 0)) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/*
	 * Global reclaim will swap to prevent OOM even with no
	 * swappiness, but memcg users want to use this knob to
	 * disable swapping for individual groups completely when
	 * using the memory controller's swap limit feature would be
	 * too expensive.
	 */
	if (!global_reclaim(sc) && !swappiness) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/*
	 * Do not apply any pressure balancing cleverness when the
	 * system is close to OOM, scan both anon and file equally
	 * (unless the swappiness setting disagrees with swapping).
	 */
	if (!sc->priority && swappiness) {
		scan_balance = SCAN_EQUAL;
		goto out;
	}

	/*
	 * Prevent the reclaimer from falling into the cache trap: as
	 * cache pages start out inactive, every cache fault will tip
	 * the scan balance towards the file LRU.  And as the file LRU
	 * shrinks, so does the window for rotation from references.
	 * This means we have a runaway feedback loop where a tiny
	 * thrashing file LRU becomes infinitely more attractive than
	 * anon pages.  Try to detect this based on file LRU size.
	 */
	if (global_reclaim(sc)) {
		unsigned long zonefile;
		unsigned long zonefree;

		zonefree = zone_page_state(zone, NR_FREE_PAGES);
		zonefile = zone_page_state(zone, NR_ACTIVE_FILE) +
			   zone_page_state(zone, NR_INACTIVE_FILE);

		if (unlikely(zonefile + zonefree <= high_wmark_pages(zone))) {
			scan_balance = SCAN_ANON;
			goto out;
		}
	}

	if (current_is_kswapd() && need_memory_boosting(zone, true)) {
		scan_balance = SCAN_FILE;
		goto out;
	}	

	/*
	 * If there is enough inactive page cache, i.e. if the size of the
	 * inactive list is greater than that of the active list *and* the
	 * inactive list actually has some pages to scan on this priority, we
	 * do not reclaim anything from the anonymous working set right now.
	 * Without the second condition we could end up never scanning an
	 * lruvec even if it has plenty of old anonymous pages unless the
	 * system is under heavy pressure.
	 */
	if (!IS_ENABLED(CONFIG_BALANCE_ANON_FILE_RECLAIM) &&
	    !inactive_file_is_low(lruvec) &&
	    get_lru_size(lruvec, LRU_INACTIVE_FILE) >> sc->priority) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	scan_balance = SCAN_FRACT;

	/*
	 * With swappiness at 100, anonymous and file have the same priority.
	 * This scanning priority is essentially the inverse of IO cost.
	 */
	anon_prio = swappiness;
	file_prio = 200 - anon_prio;

	/*
	 * OK, so we have swap space and a fair amount of page cache
	 * pages.  We use the recently rotated / recently scanned
	 * ratios to determine how valuable each cache is.
	 *
	 * Because workloads change over time (and to avoid overflow)
	 * we keep these statistics as a floating average, which ends
	 * up weighing recent references more than old ones.
	 *
	 * anon in [0], file in [1]
	 */

	anon  = get_lru_size(lruvec, LRU_ACTIVE_ANON) +
		get_lru_size(lruvec, LRU_INACTIVE_ANON);
	file  = get_lru_size(lruvec, LRU_ACTIVE_FILE) +
		get_lru_size(lruvec, LRU_INACTIVE_FILE);

	spin_lock_irq(&zone->lru_lock);
	if (unlikely(reclaim_stat->recent_scanned[0] > anon / 4)) {
		reclaim_stat->recent_scanned[0] /= 2;
		reclaim_stat->recent_rotated[0] /= 2;
	}

	if (unlikely(reclaim_stat->recent_scanned[1] > file / 4)) {
		reclaim_stat->recent_scanned[1] /= 2;
		reclaim_stat->recent_rotated[1] /= 2;
	}

	/*
	 * The amount of pressure on anon vs file pages is inversely
	 * proportional to the fraction of recently scanned pages on
	 * each list that were recently referenced and in active use.
	 */
	ap = anon_prio * (reclaim_stat->recent_scanned[0] + 1);
	ap /= reclaim_stat->recent_rotated[0] + 1;

	fp = file_prio * (reclaim_stat->recent_scanned[1] + 1);
	fp /= reclaim_stat->recent_rotated[1] + 1;
	spin_unlock_irq(&zone->lru_lock);

	fraction[0] = ap;
	fraction[1] = fp;
	denominator = ap + fp + 1;
out:
	some_scanned = false;
	/* Only use force_scan on second pass. */
	for (pass = 0; !some_scanned && pass < 2; pass++) {
		*lru_pages = 0;
		for_each_evictable_lru(lru) {
			int file = is_file_lru(lru);
			unsigned long size;
			unsigned long scan;

			size = get_lru_size(lruvec, lru);
			scan = size >> sc->priority;

			if (!scan && pass && force_scan)
				scan = min(size, SWAP_CLUSTER_MAX);

			switch (scan_balance) {
			case SCAN_EQUAL:
				/* Scan lists relative to size */
				break;
			case SCAN_FRACT:
				/*
				 * Scan types proportional to swappiness and
				 * their relative recent reclaim efficiency.
				 */
				scan = div64_u64(scan * fraction[file],
							denominator);
				break;
			case SCAN_FILE:
			case SCAN_ANON:
				/* Scan one type exclusively */
				if ((scan_balance == SCAN_FILE) != file) {
					size = 0;
					scan = 0;
				}
				break;
			default:
				/* Look ma, no brain */
				BUG();
			}

			*lru_pages += size;
			nr[lru] = scan;

			/*
			 * Skip the second pass and don't force_scan,
			 * if we found something to scan.
			 */
			some_scanned |= !!scan;
		}
	}
}

/*
 * This is a basic per-zone page freer.  Used by both kswapd and direct reclaim.
 */
#ifdef CONFIG_LRU_GEN

#ifdef CONFIG_LRU_GEN_ENABLED
DEFINE_STATIC_KEY_ARRAY_TRUE(lru_gen_caps, NR_LRU_GEN_CAPS);
#define get_cap(cap)	static_branch_likely(&lru_gen_caps[cap])
#else
DEFINE_STATIC_KEY_ARRAY_FALSE(lru_gen_caps, NR_LRU_GEN_CAPS);
#define get_cap(cap)	static_branch_unlikely(&lru_gen_caps[cap])
#endif

/*
 * 4.4 backport helpers: this tree keeps lruvecs per (memcg, zone) and
 * has neither memory demotion controls nor per-memcg vm events. The
 * target is single-node with an unpopulated ZONE_MOVABLE, so every
 * lruvec lookup below resolves to the memcg's ZONE_NORMAL lruvec.
 */
static bool can_demote(int nid, struct scan_control *sc)
{
	return false;
}

static inline long mem_cgroup_get_nr_swap_pages(struct mem_cgroup *memcg)
{
	return get_nr_swap_pages();
}

#define __count_memcg_events(memcg, item, count) do { (void)(memcg); (void)(item); (void)(count); } while (0)

static struct lruvec *mem_cgroup_lruvec(struct mem_cgroup *memcg,
					struct pglist_data *pgdat)
{
#ifdef CONFIG_MEMCG
	if (memcg)
		return mem_cgroup_zone_lruvec(&pgdat->node_zones[ZONE_NORMAL], memcg);
#endif
	VM_WARN_ON_ONCE(!mem_cgroup_disabled());

	return &pgdat->node_zones[ZONE_NORMAL].lruvec;
}

static void set_task_reclaim_state(struct task_struct *task,
				   struct reclaim_state *rs)
{
	/* Check for an overwrite */
	WARN_ON_ONCE(rs && task->reclaim_state);

	/* Check for the going away */
	WARN_ON_ONCE(!rs && !task->reclaim_state);

	task->reclaim_state = rs;
}

/*
 * 4.4: no hardware PTE access-flag management (TCR.HA is off; the
 * ARM64_HW_AF support is 4.5+ and this SoC is ARMv8.0), so the mm
 * walker's young-bit capability stays off.
 */
static inline bool arch_has_hw_pte_young(void)
{
	return false;
}

/* 5.15's cgroup_reclaim(); 4.4 keeps target_mem_cgroup on the sc. */
static bool cgroup_reclaim(struct scan_control *sc)
{
	return sc->target_mem_cgroup;
}

/*
 * Samsung's CMA skip (5.15.189 mm/vmscan.c). 4.4 spells
 * gfp_migratetype() as gfpflags_to_migratetype().
 */
#ifdef CONFIG_CMA
static bool skip_cma(struct page *page, struct scan_control *sc)
{
	return !current_is_kswapd() &&
			gfpflags_to_migratetype(sc->gfp_mask) != MIGRATE_MOVABLE &&
			get_pageblock_migratetype(page) == MIGRATE_CMA;
}
#else
static bool skip_cma(struct page *page, struct scan_control *sc)
{
	return false;
}
#endif

/******************************************************************************
 *                          shorthand helpers
 ******************************************************************************/

#define LRU_REFS_FLAGS	(BIT(PG_referenced) | BIT(PG_workingset))

#define DEFINE_MAX_SEQ(lruvec)						\
	unsigned long max_seq = READ_ONCE((lruvec)->lrugen.max_seq)

#define DEFINE_MIN_SEQ(lruvec)						\
	unsigned long min_seq[ANON_AND_FILE] = {			\
		READ_ONCE((lruvec)->lrugen.min_seq[LRU_GEN_ANON]),	\
		READ_ONCE((lruvec)->lrugen.min_seq[LRU_GEN_FILE]),	\
	}

#define for_each_gen_type_zone(gen, type, zone)				\
	for ((gen) = 0; (gen) < MAX_NR_GENS; (gen)++)			\
		for ((type) = 0; (type) < ANON_AND_FILE; (type)++)	\
			for ((zone) = 0; (zone) < MAX_NR_ZONES; (zone)++)

static struct lruvec *get_lruvec(struct mem_cgroup *memcg, int nid)
{
	struct pglist_data *pgdat = NODE_DATA(nid);

#ifdef CONFIG_MEMCG
	if (memcg)
		return mem_cgroup_zone_lruvec(&pgdat->node_zones[ZONE_NORMAL], memcg);
#endif
	VM_WARN_ON_ONCE(!mem_cgroup_disabled());

	return &pgdat->node_zones[ZONE_NORMAL].lruvec;
}

static int get_swappiness(struct lruvec *lruvec, struct scan_control *sc)
{
	int swappiness;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	if (!can_demote(pgdat->node_id, sc) &&
		mem_cgroup_get_nr_swap_pages(memcg) <= 0)
		return 0;

	swappiness = mem_cgroup_swappiness(memcg);

	return swappiness;
}

static int get_nr_gens(struct lruvec *lruvec, int type)
{
	return lruvec->lrugen.max_seq - lruvec->lrugen.min_seq[type] + 1;
}

static bool __maybe_unused seq_is_valid(struct lruvec *lruvec)
{
	/* see the comment on lru_gen_struct */
	return get_nr_gens(lruvec, LRU_GEN_FILE) >= MIN_NR_GENS &&
	       get_nr_gens(lruvec, LRU_GEN_FILE) <= get_nr_gens(lruvec, LRU_GEN_ANON) &&
	       get_nr_gens(lruvec, LRU_GEN_ANON) <= MAX_NR_GENS;
}

/******************************************************************************
 *                          mm_struct list
 ******************************************************************************/

static struct lru_gen_mm_list *get_mm_list(struct mem_cgroup *memcg)
{
	static struct lru_gen_mm_list mm_list = {
		.fifo = LIST_HEAD_INIT(mm_list.fifo),
		.lock = __SPIN_LOCK_UNLOCKED(mm_list.lock),
	};

#ifdef CONFIG_MEMCG
	if (memcg)
		return &memcg->mm_list;
#endif
	VM_WARN_ON_ONCE(!mem_cgroup_disabled());

	return &mm_list;
}

void lru_gen_add_mm(struct mm_struct *mm)
{
	int nid;
	struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);

	VM_WARN_ON_ONCE(!list_empty(&mm->lru_gen.list));
#ifdef CONFIG_MEMCG
	VM_WARN_ON_ONCE(mm->lru_gen.memcg);
	mm->lru_gen.memcg = memcg;
#endif
	spin_lock(&mm_list->lock);

	for_each_node_state(nid, N_MEMORY) {
		struct lruvec *lruvec = get_lruvec(memcg, nid);

		if (!lruvec)
			continue;

		/* the first addition since the last iteration */
		if (lruvec->mm_state.tail == &mm_list->fifo)
			lruvec->mm_state.tail = &mm->lru_gen.list;
	}

	list_add_tail(&mm->lru_gen.list, &mm_list->fifo);

	spin_unlock(&mm_list->lock);
}

void lru_gen_del_mm(struct mm_struct *mm)
{
	int nid;
	struct lru_gen_mm_list *mm_list;
	struct mem_cgroup *memcg = NULL;

	if (list_empty(&mm->lru_gen.list))
		return;

#ifdef CONFIG_MEMCG
	memcg = mm->lru_gen.memcg;
#endif
	mm_list = get_mm_list(memcg);

	spin_lock(&mm_list->lock);

	for_each_node(nid) {
		struct lruvec *lruvec = get_lruvec(memcg, nid);

		if (!lruvec)
			continue;

		/* where the current iteration continues after */
		if (lruvec->mm_state.head == &mm->lru_gen.list)
			lruvec->mm_state.head = lruvec->mm_state.head->prev;

		/* where the last iteration ended before */
		if (lruvec->mm_state.tail == &mm->lru_gen.list)
			lruvec->mm_state.tail = lruvec->mm_state.tail->next;
	}

	list_del_init(&mm->lru_gen.list);

	spin_unlock(&mm_list->lock);

#ifdef CONFIG_MEMCG
	mem_cgroup_put(mm->lru_gen.memcg);
	mm->lru_gen.memcg = NULL;
#endif
}

#ifdef CONFIG_MEMCG
void lru_gen_migrate_mm(struct mm_struct *mm)
{
	struct mem_cgroup *memcg;
	struct task_struct *task = rcu_dereference_protected(mm->owner, true);

	VM_WARN_ON_ONCE(task->mm != mm);
	lockdep_assert_held(&task->alloc_lock);

	/* for mm_update_next_owner() */
	if (mem_cgroup_disabled())
		return;

	/* migration can happen before addition */
	if (!mm->lru_gen.memcg)
		return;

	rcu_read_lock();
	memcg = mem_cgroup_from_task(task);
	rcu_read_unlock();
	if (memcg == mm->lru_gen.memcg)
		return;

	VM_WARN_ON_ONCE(list_empty(&mm->lru_gen.list));

	lru_gen_del_mm(mm);
	lru_gen_add_mm(mm);
}
#endif

/*
 * Bloom filters with m=1<<15, k=2 and the false positive rates of ~1/5 when
 * n=10,000 and ~1/2 when n=20,000, where, conventionally, m is the number of
 * bits in a bitmap, k is the number of hash functions and n is the number of
 * inserted items.
 *
 * Page table walkers use one of the two filters to reduce their search space.
 * To get rid of non-leaf entries that no longer have enough leaf entries, the
 * aging uses the double-buffering technique to flip to the other filter each
 * time it produces a new generation. For non-leaf entries that have enough
 * leaf entries, the aging carries them over to the next generation in
 * walk_pmd_range(); the eviction also report them when walking the rmap
 * in lru_gen_look_around().
 *
 * For future optimizations:
 * 1. It's not necessary to keep both filters all the time. The spare one can be
 *    freed after the RCU grace period and reallocated if needed again.
 * 2. And when reallocating, it's worth scaling its size according to the number
 *    of inserted entries in the other filter, to reduce the memory overhead on
 *    small systems and false positives on large systems.
 * 3. Jenkins' hash function is an alternative to Knuth's.
 */
#define BLOOM_FILTER_SHIFT	15

static inline int filter_gen_from_seq(unsigned long seq)
{
	return seq % NR_BLOOM_FILTERS;
}

static void get_item_key(void *item, int *key)
{
	u32 hash = hash_ptr(item, BLOOM_FILTER_SHIFT * 2);

	BUILD_BUG_ON(BLOOM_FILTER_SHIFT * 2 > BITS_PER_TYPE(u32));

	key[0] = hash & (BIT(BLOOM_FILTER_SHIFT) - 1);
	key[1] = hash >> BLOOM_FILTER_SHIFT;
}

static void reset_bloom_filter(struct lruvec *lruvec, unsigned long seq)
{
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = lruvec->mm_state.filters[gen];
	if (filter) {
		bitmap_clear(filter, 0, BIT(BLOOM_FILTER_SHIFT));
		return;
	}

	filter = bitmap_zalloc(BIT(BLOOM_FILTER_SHIFT),
			       __GFP_HIGH | __GFP_NOMEMALLOC | __GFP_NOWARN);
	WRITE_ONCE(lruvec->mm_state.filters[gen], filter);
}

static void update_bloom_filter(struct lruvec *lruvec, unsigned long seq, void *item)
{
	int key[2];
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = READ_ONCE(lruvec->mm_state.filters[gen]);
	if (!filter)
		return;

	get_item_key(item, key);

	if (!test_bit(key[0], filter))
		set_bit(key[0], filter);
	if (!test_bit(key[1], filter))
		set_bit(key[1], filter);
}

static bool test_bloom_filter(struct lruvec *lruvec, unsigned long seq, void *item)
{
	int key[2];
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = READ_ONCE(lruvec->mm_state.filters[gen]);
	if (!filter)
		return true;

	get_item_key(item, key);

	return test_bit(key[0], filter) && test_bit(key[1], filter);
}

static void reset_mm_stats(struct lruvec *lruvec, struct lru_gen_mm_walk *walk, bool last)
{
	int i;
	int hist;

	lockdep_assert_held(&get_mm_list(lruvec_memcg(lruvec))->lock);

	if (walk) {
		hist = lru_hist_from_seq(walk->max_seq);

		for (i = 0; i < NR_MM_STATS; i++) {
			WRITE_ONCE(lruvec->mm_state.stats[hist][i],
				   lruvec->mm_state.stats[hist][i] + walk->mm_stats[i]);
			walk->mm_stats[i] = 0;
		}
	}

	if (NR_HIST_GENS > 1 && last) {
		hist = lru_hist_from_seq(lruvec->mm_state.seq + 1);

		for (i = 0; i < NR_MM_STATS; i++)
			WRITE_ONCE(lruvec->mm_state.stats[hist][i], 0);
	}
}

static bool should_skip_mm(struct mm_struct *mm, struct lru_gen_mm_walk *walk)
{
	int type;
	unsigned long size = 0;
	struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);
	int key = pgdat->node_id;

	if (!walk->full_scan && !node_isset(key, mm->lru_gen.nodes))
		return true;

	node_clear(key, mm->lru_gen.nodes);

	for (type = !walk->can_swap; type < ANON_AND_FILE; type++) {
		/* 4.4: shmem pages are accounted in MM_ANONPAGES */
		size += type ? get_mm_counter(mm, MM_FILEPAGES) :
			       get_mm_counter(mm, MM_ANONPAGES);
	}

	if (size < MIN_LRU_BATCH)
		return true;

	return !mmget_not_zero(mm);
}

static bool iterate_mm_list(struct lruvec *lruvec, struct lru_gen_mm_walk *walk,
			    struct mm_struct **iter)
{
	bool first = false;
	bool last = false;
	struct mm_struct *mm = NULL;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);
	struct lru_gen_mm_state *mm_state = &lruvec->mm_state;

	/*
	 * mm_state->seq is incremented after each iteration of mm_list. There
	 * are three interesting cases for this page table walker:
	 * 1. It tries to start a new iteration with a stale max_seq: there is
	 *    nothing left to do.
	 * 2. It started the next iteration: it needs to reset the Bloom filter
	 *    so that a fresh set of PTE tables can be recorded.
	 * 3. It ended the current iteration: it needs to reset the mm stats
	 *    counters and tell its caller to increment max_seq.
	 */
	spin_lock(&mm_list->lock);

	VM_WARN_ON_ONCE(mm_state->seq + 1 < walk->max_seq);

	if (walk->max_seq <= mm_state->seq)
		goto done;

	if (!mm_state->head)
		mm_state->head = &mm_list->fifo;

	if (mm_state->head == &mm_list->fifo)
		first = true;

	do {
		mm_state->head = mm_state->head->next;
		if (mm_state->head == &mm_list->fifo) {
			WRITE_ONCE(mm_state->seq, mm_state->seq + 1);
			last = true;
			break;
		}

		/* force scan for those added after the last iteration */
		if (!mm_state->tail || mm_state->tail == mm_state->head) {
			mm_state->tail = mm_state->head->next;
			walk->full_scan = true;
		}

		mm = list_entry(mm_state->head, struct mm_struct, lru_gen.list);
		if (should_skip_mm(mm, walk))
			mm = NULL;
	} while (!mm);
done:
	if (*iter || last)
		reset_mm_stats(lruvec, walk, last);

	spin_unlock(&mm_list->lock);

	if (mm && first)
		reset_bloom_filter(lruvec, walk->max_seq + 1);

	if (*iter)
		mmput_async(*iter);

	*iter = mm;

	return last;
}

static bool iterate_mm_list_nowalk(struct lruvec *lruvec, unsigned long max_seq)
{
	bool success = false;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);
	struct lru_gen_mm_state *mm_state = &lruvec->mm_state;

	spin_lock(&mm_list->lock);

	VM_WARN_ON_ONCE(mm_state->seq + 1 < max_seq);

	if (max_seq > mm_state->seq) {
		mm_state->head = NULL;
		mm_state->tail = NULL;
		WRITE_ONCE(mm_state->seq, mm_state->seq + 1);
		reset_mm_stats(lruvec, NULL, true);
		success = true;
	}

	spin_unlock(&mm_list->lock);

	return success;
}

/******************************************************************************
 *                          refault feedback loop
 ******************************************************************************/

/*
 * A feedback loop based on Proportional-Integral-Derivative (PID) controller.
 *
 * The P term is refaulted/(evicted+protected) from a tier in the generation
 * currently being evicted; the I term is the exponential moving average of the
 * P term over the generations previously evicted, using the smoothing factor
 * 1/2; the D term isn't supported.
 *
 * The setpoint (SP) is always the first tier of one type; the process variable
 * (PV) is either any tier of the other type or any other tier of the same
 * type.
 *
 * The error is the difference between the SP and the PV; the correction is to
 * turn off protection when SP>PV or turn on protection when SP<PV.
 *
 * For future optimizations:
 * 1. The D term may discount the other two terms over time so that long-lived
 *    generations can resist stale information.
 */
struct ctrl_pos {
	unsigned long refaulted;
	unsigned long total;
	int gain;
};

static void read_ctrl_pos(struct lruvec *lruvec, int type, int tier, int gain,
			  struct ctrl_pos *pos)
{
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	int hist = lru_hist_from_seq(lrugen->min_seq[type]);

	pos->refaulted = lrugen->avg_refaulted[type][tier] +
			 atomic_long_read(&lrugen->refaulted[hist][type][tier]);
	pos->total = lrugen->avg_total[type][tier] +
		     atomic_long_read(&lrugen->evicted[hist][type][tier]);
	if (tier)
		pos->total += lrugen->protected[hist][type][tier - 1];
	pos->gain = gain;
}

static void reset_ctrl_pos(struct lruvec *lruvec, int type, bool carryover)
{
	int hist, tier;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	bool clear = carryover ? NR_HIST_GENS == 1 : NR_HIST_GENS > 1;
	unsigned long seq = carryover ? lrugen->min_seq[type] : lrugen->max_seq + 1;

	lockdep_assert_held(&lruvec_zone(lruvec)->lru_lock);

	if (!carryover && !clear)
		return;

	hist = lru_hist_from_seq(seq);

	for (tier = 0; tier < MAX_NR_TIERS; tier++) {
		if (carryover) {
			unsigned long sum;

			sum = lrugen->avg_refaulted[type][tier] +
			      atomic_long_read(&lrugen->refaulted[hist][type][tier]);
			WRITE_ONCE(lrugen->avg_refaulted[type][tier], sum / 2);

			sum = lrugen->avg_total[type][tier] +
			      atomic_long_read(&lrugen->evicted[hist][type][tier]);
			if (tier)
				sum += lrugen->protected[hist][type][tier - 1];
			WRITE_ONCE(lrugen->avg_total[type][tier], sum / 2);
		}

		if (clear) {
			atomic_long_set(&lrugen->refaulted[hist][type][tier], 0);
			atomic_long_set(&lrugen->evicted[hist][type][tier], 0);
			if (tier)
				WRITE_ONCE(lrugen->protected[hist][type][tier - 1], 0);
		}
	}
}

static bool positive_ctrl_err(struct ctrl_pos *sp, struct ctrl_pos *pv)
{
	/*
	 * Return true if the PV has a limited number of refaults or a lower
	 * refaulted/total than the SP.
	 */
	return pv->refaulted < MIN_LRU_BATCH ||
	       pv->refaulted * (sp->total + MIN_LRU_BATCH) * sp->gain <=
	       (sp->refaulted + 1) * pv->total * pv->gain;
}

/******************************************************************************
 *                          the aging
 ******************************************************************************/

/* promote pages accessed through page tables */
static int page_update_gen(struct page *page, int gen)
{
	unsigned long new_flags, old_flags = READ_ONCE(page->flags);

	VM_WARN_ON_ONCE(gen >= MAX_NR_GENS);
	VM_WARN_ON_ONCE(!rcu_read_lock_held());

	do {
		/* lru_gen_del_page() has isolated this page? */
		if (!(old_flags & LRU_GEN_MASK)) {
			/* for shrink_page_list() */
			new_flags = old_flags | BIT(PG_referenced);
			continue;
		}

		new_flags = old_flags & ~(LRU_GEN_MASK | LRU_REFS_MASK | LRU_REFS_FLAGS);
		new_flags |= (gen + 1UL) << LRU_GEN_PGOFF;
	} while (!try_cmpxchg(&page->flags, &old_flags, new_flags));

	return ((old_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
}

/* protect pages accessed multiple times through file descriptors */
static int page_inc_gen(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	int type = page_is_file_cache(page);
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	int new_gen, old_gen = lru_gen_from_seq(lrugen->min_seq[type]);
	unsigned long new_flags, old_flags = READ_ONCE(page->flags);

	VM_WARN_ON_ONCE_PAGE(!(old_flags & LRU_GEN_MASK), page);

	do {
		new_gen = ((old_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
		/* page_update_gen() has promoted this page? */
		if (new_gen >= 0 && new_gen != old_gen)
			return new_gen;

		new_gen = (old_gen + 1) % MAX_NR_GENS;

		new_flags = old_flags & ~(LRU_GEN_MASK | LRU_REFS_MASK | LRU_REFS_FLAGS);
		new_flags |= (new_gen + 1UL) << LRU_GEN_PGOFF;
		/* for end_page_writeback() */
		if (reclaiming)
			new_flags |= BIT(PG_reclaim);
	} while (!try_cmpxchg(&page->flags, &old_flags, new_flags));

	lru_gen_update_size(lruvec, page, old_gen, new_gen);

	return new_gen;
}

static void update_batch_size(struct lru_gen_mm_walk *walk, struct page *page,
			      int old_gen, int new_gen)
{
	int type = page_is_file_cache(page);
	int zone = page_zonenum(page);
	int delta = hpage_nr_pages(page);

	VM_WARN_ON_ONCE(old_gen >= MAX_NR_GENS);
	VM_WARN_ON_ONCE(new_gen >= MAX_NR_GENS);

	walk->batched++;

	walk->nr_pages[old_gen][type][zone] -= delta;
	walk->nr_pages[new_gen][type][zone] += delta;
}

static void reset_batch_size(struct lruvec *lruvec, struct lru_gen_mm_walk *walk)
{
	int gen, type, zone;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	walk->batched = 0;

	for_each_gen_type_zone(gen, type, zone) {
		enum lru_list lru = type * LRU_INACTIVE_FILE;
		int delta = walk->nr_pages[gen][type][zone];

		if (!delta)
			continue;

		walk->nr_pages[gen][type][zone] = 0;
		WRITE_ONCE(lrugen->nr_pages[gen][type][zone],
			   lrugen->nr_pages[gen][type][zone] + delta);

		if (lru_gen_is_active(lruvec, gen))
			lru += LRU_ACTIVE;
		__update_lru_size(lruvec, lru, zone, delta);
	}
}

static int should_skip_vma(unsigned long start, unsigned long end, struct mm_walk *args)
{
	struct address_space *mapping;
	struct vm_area_struct *vma = args->vma;
	struct lru_gen_mm_walk *walk = args->private;

	if (!vma_is_accessible(vma))
		return true;

	if (is_vm_hugetlb_page(vma))
		return true;

	if (vma->vm_flags & (VM_LOCKED | VM_SPECIAL | VM_SEQ_READ | VM_RAND_READ))
		return true;

	if (vma == get_gate_vma(vma->vm_mm))
		return true;

	if (vma_is_anonymous(vma))
		return !walk->can_swap;

	if (WARN_ON_ONCE(!vma->vm_file || !vma->vm_file->f_mapping))
		return true;

	mapping = vma->vm_file->f_mapping;
	if (mapping_unevictable(mapping))
		return true;

	if (shmem_mapping(mapping))
		return !walk->can_swap;

	/* to exclude special mappings like dax, etc. */
	return !mapping->a_ops->readpage;
}

/*
 * Some userspace memory allocators map many single-page VMAs. Instead of
 * returning back to the PGD table for each of such VMAs, finish an entire PMD
 * table to reduce zigzags and improve cache performance.
 */
static bool get_next_vma(unsigned long mask, unsigned long size, struct mm_walk *args,
			 unsigned long *vm_start, unsigned long *vm_end)
{
	unsigned long start = round_up(*vm_end, size);
	unsigned long end = (start | ~mask) + 1;

	VM_WARN_ON_ONCE(mask & size);
	VM_WARN_ON_ONCE((start & mask) != (*vm_start & mask));

	while (args->vma) {
		if (start >= args->vma->vm_end) {
			args->vma = args->vma->vm_next;
			continue;
		}

		if (end && end <= args->vma->vm_start)
			return false;

		if (should_skip_vma(args->vma->vm_start, args->vma->vm_end, args)) {
			args->vma = args->vma->vm_next;
			continue;
		}

		*vm_start = max(start, args->vma->vm_start);
		*vm_end = min(end - 1, args->vma->vm_end - 1) + 1;

		return true;
	}

	return false;
}

static unsigned long get_pte_pfn(pte_t pte, struct vm_area_struct *vma, unsigned long addr)
{
	unsigned long pfn = pte_pfn(pte);

	VM_WARN_ON_ONCE(addr < vma->vm_start || addr >= vma->vm_end);

	if (!pte_present(pte) || is_zero_pfn(pfn))
		return -1;

	/* 4.4: arm64 has no ZONE_DEVICE; only pte_special can appear */
	if (WARN_ON_ONCE(pte_special(pte)))
		return -1;

	if (WARN_ON_ONCE(!pfn_valid(pfn)))
		return -1;

	return pfn;
}

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) || defined(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG)
static unsigned long get_pmd_pfn(pmd_t pmd, struct vm_area_struct *vma, unsigned long addr)
{
	unsigned long pfn = pmd_pfn(pmd);

	VM_WARN_ON_ONCE(addr < vma->vm_start || addr >= vma->vm_end);

	if (!pmd_present(pmd) || is_huge_zero_pmd(pmd))
		return -1;

	if (WARN_ON_ONCE(pmd_devmap(pmd)))
		return -1;

	if (WARN_ON_ONCE(!pfn_valid(pfn)))
		return -1;

	return pfn;
}
#endif

static struct page *get_pfn_page(unsigned long pfn, struct mem_cgroup *memcg,
				 struct pglist_data *pgdat, bool can_swap)
{
	struct page *page;

	/* try to avoid unnecessary memory loads */
	if (pfn < pgdat->node_start_pfn || pfn >= pgdat_end_pfn(pgdat))
		return NULL;

	page = compound_head(pfn_to_page(pfn));
	if (page_to_nid(page) != pgdat->node_id)
		return NULL;

	if (page_memcg_rcu(page) != memcg)
		return NULL;

	/* file VMAs can contain anon pages from COW */
	if (!page_is_file_cache(page) && !can_swap)
		return NULL;

	return page;
}

static bool suitable_to_scan(int total, int young)
{
	int n = clamp_t(int, cache_line_size() / sizeof(pte_t), 2, 8);

	/* suitable if the average number of young PTEs per cacheline is >=1 */
	return young * n >= total;
}

static bool walk_pte_range(pmd_t *pmd, unsigned long start, unsigned long end,
			   struct mm_walk *args)
{
	int i;
	pte_t *pte;
	spinlock_t *ptl;
	unsigned long addr;
	int total = 0;
	int young = 0;
	struct lru_gen_mm_walk *walk = args->private;
	struct mem_cgroup *memcg = lruvec_memcg(walk->lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);
	int old_gen, new_gen = lru_gen_from_seq(walk->max_seq);

	/* 4.4: THP is disabled and hugetlb is off -- no leaf pmd here */

	ptl = pte_lockptr(args->mm, pmd);
	if (!spin_trylock(ptl))
		return false;

	arch_enter_lazy_mmu_mode();

	pte = pte_offset_map(pmd, start & PMD_MASK);
restart:
	for (i = pte_index(start), addr = start; addr != end; i++, addr += PAGE_SIZE) {
		unsigned long pfn;
		struct page *page;

		total++;
		walk->mm_stats[MM_LEAF_TOTAL]++;

		pfn = get_pte_pfn(pte[i], args->vma, addr);
		if (pfn == -1)
			continue;

		if (!pte_young(pte[i])) {
			walk->mm_stats[MM_LEAF_OLD]++;
			continue;
		}

		page = get_pfn_page(pfn, memcg, pgdat, walk->can_swap);
		if (!page)
			continue;

		if (!ptep_test_and_clear_young(args->vma, addr, pte + i))
			VM_WARN_ON_ONCE(true);

		young++;
		walk->mm_stats[MM_LEAF_YOUNG]++;

		if (pte_dirty(pte[i]) && !PageDirty(page) &&
		    !(PageAnon(page) && PageSwapBacked(page) &&
		      !PageSwapCache(page)))
			set_page_dirty(page);

		old_gen = page_update_gen(page, new_gen);
		if (old_gen >= 0 && old_gen != new_gen)
			update_batch_size(walk, page, old_gen, new_gen);
	}

	if (i < PTRS_PER_PTE && get_next_vma(PMD_MASK, PAGE_SIZE, args, &start, &end))
		goto restart;

	pte_unmap(pte);

	arch_leave_lazy_mmu_mode();
	spin_unlock(ptl);

	return suitable_to_scan(total, young);
}

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) || defined(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG)
static void walk_pmd_range_locked(pud_t *pud, unsigned long next, struct vm_area_struct *vma,
				  struct mm_walk *args, unsigned long *bitmap, unsigned long *start)
{
	int i;
	pmd_t *pmd;
	spinlock_t *ptl;
	struct lru_gen_mm_walk *walk = args->private;
	struct mem_cgroup *memcg = lruvec_memcg(walk->lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);
	int old_gen, new_gen = lru_gen_from_seq(walk->max_seq);

	/* 4.4: the PUD is folded on arm64; its entries are never leaves */

	/* try to batch at most 1+MIN_LRU_BATCH+1 entries */
	if (*start == -1) {
		*start = next;
		return;
	}

	i = next == -1 ? 0 : pmd_index(next) - pmd_index(*start);
	if (i && i <= MIN_LRU_BATCH) {
		__set_bit(i - 1, bitmap);
		return;
	}

	pmd = pmd_offset(pud, *start);

	ptl = pmd_lockptr(args->mm, pmd);
	if (!spin_trylock(ptl))
		goto done;

	arch_enter_lazy_mmu_mode();

	do {
		unsigned long pfn;
		struct page *page;
		unsigned long addr = i ? (*start & PMD_MASK) + i * PMD_SIZE : *start;

		pfn = get_pmd_pfn(pmd[i], vma, addr);
		if (pfn == -1)
			goto next;

		if (!pmd_trans_huge(pmd[i])) {
			if (IS_ENABLED(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG) &&
			    get_cap(LRU_GEN_NONLEAF_YOUNG))
				pmdp_test_and_clear_young(vma, addr, pmd + i);
			goto next;
		}

		page = get_pfn_page(pfn, memcg, pgdat, walk->can_swap);
		if (!page)
			goto next;

		if (!pmdp_test_and_clear_young(vma, addr, pmd + i))
			goto next;

		walk->mm_stats[MM_LEAF_YOUNG]++;

		if (pmd_dirty(pmd[i]) && !PageDirty(page) &&
		    !(PageAnon(page) && PageSwapBacked(page) &&
		      !PageSwapCache(page)))
			set_page_dirty(page);

		old_gen = page_update_gen(page, new_gen);
		if (old_gen >= 0 && old_gen != new_gen)
			update_batch_size(walk, page, old_gen, new_gen);
next:
		i = i > MIN_LRU_BATCH ? 0 : find_next_bit(bitmap, MIN_LRU_BATCH, i) + 1;
	} while (i <= MIN_LRU_BATCH);

	arch_leave_lazy_mmu_mode();
	spin_unlock(ptl);
done:
	*start = -1;
	bitmap_zero(bitmap, MIN_LRU_BATCH);
}
#else
static void walk_pmd_range_locked(pud_t *pud, unsigned long next, struct vm_area_struct *vma,
				  struct mm_walk *args, unsigned long *bitmap, unsigned long *start)
{
}
#endif

static void walk_pmd_range(pud_t *pud, unsigned long start, unsigned long end,
			   struct mm_walk *args)
{
	int i;
	pmd_t *pmd;
	unsigned long next;
	unsigned long addr;
	struct vm_area_struct *vma;
	unsigned long pos = -1;
	struct lru_gen_mm_walk *walk = args->private;
	unsigned long bitmap[BITS_TO_LONGS(MIN_LRU_BATCH)] = {};

	/* 4.4: the PUD is folded on arm64; its entries are never leaves */

	/*
	 * Finish an entire PMD in two passes: the first only reaches to PTE
	 * tables to avoid taking the PMD lock; the second, if necessary, takes
	 * the PMD lock to clear the accessed bit in PMD entries.
	 */
	pmd = pmd_offset(pud, start & PUD_MASK);
restart:
	/* walk_pte_range() may call get_next_vma() */
	vma = args->vma;
	for (i = pmd_index(start), addr = start; addr != end; i++, addr = next) {
		pmd_t val = pmd_read_atomic(pmd + i);

		/* for pmd_read_atomic() */
		barrier();

		next = pmd_addr_end(addr, end);

		if (!pmd_present(val) || is_huge_zero_pmd(val)) {
			walk->mm_stats[MM_LEAF_TOTAL]++;
			continue;
		}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		if (pmd_trans_huge(val)) {
			unsigned long pfn = pmd_pfn(val);
			struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);

			walk->mm_stats[MM_LEAF_TOTAL]++;

			if (!pmd_young(val)) {
				walk->mm_stats[MM_LEAF_OLD]++;
				continue;
			}

			/* try to avoid unnecessary memory loads */
			if (pfn < pgdat->node_start_pfn || pfn >= pgdat_end_pfn(pgdat))
				continue;

			walk_pmd_range_locked(pud, addr, vma, args, bitmap, &pos);
			continue;
		}
#endif
		walk->mm_stats[MM_NONLEAF_TOTAL]++;

#ifdef CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG
		if (get_cap(LRU_GEN_NONLEAF_YOUNG)) {
			if (!pmd_young(val))
				continue;

			walk_pmd_range_locked(pud, addr, vma, args, bitmap, &pos);
		}
#endif
		if (!walk->full_scan && !test_bloom_filter(walk->lruvec, walk->max_seq, pmd + i))
			continue;

		walk->mm_stats[MM_NONLEAF_FOUND]++;

		if (!walk_pte_range(&val, addr, next, args))
			continue;

		walk->mm_stats[MM_NONLEAF_ADDED]++;

		/* carry over to the next generation */
		update_bloom_filter(walk->lruvec, walk->max_seq + 1, pmd + i);
	}

	walk_pmd_range_locked(pud, -1, vma, args, bitmap, &pos);

	if (i < PTRS_PER_PMD && get_next_vma(PUD_MASK, PMD_SIZE, args, &start, &end))
		goto restart;
}

static int walk_pud_range(p4d_t *p4d, unsigned long start, unsigned long end,
			  struct mm_walk *args)
{
	int i;
	pud_t *pud;
	unsigned long addr;
	unsigned long next;
	struct lru_gen_mm_walk *walk = args->private;

	/* 4.4: the p4d is folded on arm64; its entries are never leaves */

	pud = pud_offset(p4d, start & P4D_MASK);
restart:
	/* 4.4: the folded PUD has exactly one entry (PTRS_PER_PUD == 1) */
	for (i = 0, addr = start; addr != end; i++, addr = next) {
		pud_t val = READ_ONCE(pud[i]);

		next = pud_addr_end(addr, end);

		if (!pud_present(val))
			continue;

		walk_pmd_range(&val, addr, next, args);

		if (need_resched() || walk->batched >= MAX_LRU_BATCH) {
			end = (addr | ~PUD_MASK) + 1;
			goto done;
		}
	}

	if (i < PTRS_PER_PUD && get_next_vma(P4D_MASK, PUD_SIZE, args, &start, &end))
		goto restart;

	end = round_up(end, P4D_SIZE);
done:
	if (!end || !args->vma)
		return 1;

	walk->next_addr = max(end, args->vma->vm_start);

	return -EAGAIN;
}

static void walk_mm(struct lruvec *lruvec, struct mm_struct *mm, struct lru_gen_mm_walk *walk)
{
	static const struct mm_walk_ops mm_walk_ops = {
		.test_walk = should_skip_vma,
		.p4d_entry = walk_pud_range,
	};

	int err;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);

	walk->next_addr = FIRST_USER_ADDRESS;

	do {
		DEFINE_MAX_SEQ(lruvec);

		err = -EBUSY;

		/* another thread might have called inc_max_seq() */
		if (walk->max_seq != max_seq)
			break;

		/* page_update_gen() requires stable page_memcg() */
		if (!mem_cgroup_trylock_pages(memcg))
			break;

		/* the caller might be holding the lock for write */
		if (down_read_trylock(&mm->mmap_sem)) {
			err = walk_page_range(mm, walk->next_addr, ULONG_MAX, &mm_walk_ops, walk);

			up_read(&mm->mmap_sem);
		}

		mem_cgroup_unlock_pages();

		if (walk->batched) {
			spin_lock_irq(&lruvec_zone(lruvec)->lru_lock);
			reset_batch_size(lruvec, walk);
			spin_unlock_irq(&lruvec_zone(lruvec)->lru_lock);
		}

		cond_resched();
	} while (err == -EAGAIN);
}

static struct lru_gen_mm_walk *set_mm_walk(struct pglist_data *pgdat)
{
	struct lru_gen_mm_walk *walk = current->reclaim_state->mm_walk;

	if (pgdat && current_is_kswapd()) {
		VM_WARN_ON_ONCE(walk);

		walk = &pgdat->mm_walk;
	} else if (!pgdat && !walk) {
		VM_WARN_ON_ONCE(current_is_kswapd());

		walk = kzalloc(sizeof(*walk), __GFP_HIGH | __GFP_NOMEMALLOC | __GFP_NOWARN);
	}

	current->reclaim_state->mm_walk = walk;

	return walk;
}

static void clear_mm_walk(void)
{
	struct lru_gen_mm_walk *walk = current->reclaim_state->mm_walk;

	VM_WARN_ON_ONCE(walk && memchr_inv(walk->nr_pages, 0, sizeof(walk->nr_pages)));
	VM_WARN_ON_ONCE(walk && memchr_inv(walk->mm_stats, 0, sizeof(walk->mm_stats)));

	current->reclaim_state->mm_walk = NULL;

	if (!current_is_kswapd())
		kfree(walk);
}

static bool inc_min_seq(struct lruvec *lruvec, int type, bool can_swap)
{
	int zone;
	int remaining = MAX_LRU_BATCH;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	int new_gen, old_gen = lru_gen_from_seq(lrugen->min_seq[type]);

	if (type == LRU_GEN_ANON && !can_swap)
		goto done;

	/* prevent cold/hot inversion if full_scan is true */
	for (zone = 0; zone < MAX_NR_ZONES; zone++) {
		struct list_head *head = &lrugen->lists[old_gen][type][zone];

		while (!list_empty(head)) {
			struct page *page = lru_to_page(head);

			VM_WARN_ON_ONCE_PAGE(PageUnevictable(page), page);
			VM_WARN_ON_ONCE_PAGE(PageActive(page), page);
			VM_WARN_ON_ONCE_PAGE(page_is_file_cache(page) != type, page);
			VM_WARN_ON_ONCE_PAGE(page_zonenum(page) != zone, page);

			new_gen = page_inc_gen(lruvec, page, false);
			list_move_tail(&page->lru, &lrugen->lists[new_gen][type][zone]);

			if (!--remaining)
				return false;
		}
	}
done:
	reset_ctrl_pos(lruvec, type, true);
	WRITE_ONCE(lrugen->min_seq[type], lrugen->min_seq[type] + 1);

	return true;
}

static bool try_to_inc_min_seq(struct lruvec *lruvec, bool can_swap)
{
	int gen, type, zone;
	bool success = false;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	DEFINE_MIN_SEQ(lruvec);

	VM_WARN_ON_ONCE(!seq_is_valid(lruvec));

	/* find the oldest populated generation */
	for (type = !can_swap; type < ANON_AND_FILE; type++) {
		while (min_seq[type] + MIN_NR_GENS <= lrugen->max_seq) {
			gen = lru_gen_from_seq(min_seq[type]);

			for (zone = 0; zone < MAX_NR_ZONES; zone++) {
				if (!list_empty(&lrugen->lists[gen][type][zone]))
					goto next;
			}

			min_seq[type]++;
		}
next:
		;
	}

	/* see the comment on lru_gen_struct */
	if (can_swap) {
		min_seq[LRU_GEN_ANON] = min(min_seq[LRU_GEN_ANON], min_seq[LRU_GEN_FILE]);
		min_seq[LRU_GEN_FILE] = max(min_seq[LRU_GEN_ANON], lrugen->min_seq[LRU_GEN_FILE]);
	}

	for (type = !can_swap; type < ANON_AND_FILE; type++) {
		if (min_seq[type] == lrugen->min_seq[type])
			continue;

		reset_ctrl_pos(lruvec, type, true);
		WRITE_ONCE(lrugen->min_seq[type], min_seq[type]);
		success = true;
	}

	return success;
}

static void inc_max_seq(struct lruvec *lruvec, bool can_swap, bool full_scan)
{
	int prev, next;
	int type, zone;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
restart:
	spin_lock_irq(&lruvec_zone(lruvec)->lru_lock);

	VM_WARN_ON_ONCE(!seq_is_valid(lruvec));

	for (type = ANON_AND_FILE - 1; type >= 0; type--) {
		if (get_nr_gens(lruvec, type) != MAX_NR_GENS)
			continue;

		VM_WARN_ON_ONCE(!full_scan && (type == LRU_GEN_FILE || can_swap));

		while (!inc_min_seq(lruvec, type, can_swap)) {
			spin_unlock_irq(&lruvec_zone(lruvec)->lru_lock);
			cond_resched();
			spin_lock_irq(&lruvec_zone(lruvec)->lru_lock);
		}
		if (inc_min_seq(lruvec, type, can_swap))
			continue;

		spin_unlock_irq(&lruvec_zone(lruvec)->lru_lock);
		cond_resched();
		goto restart;

	}

	/*
	 * Update the active/inactive LRU sizes for compatibility. Both sides of
	 * the current max_seq need to be covered, since max_seq+1 can overlap
	 * with min_seq[LRU_GEN_ANON] if swapping is constrained. And if they do
	 * overlap, cold/hot inversion happens.
	 */
	prev = lru_gen_from_seq(lrugen->max_seq - 1);
	next = lru_gen_from_seq(lrugen->max_seq + 1);

	for (type = 0; type < ANON_AND_FILE; type++) {
		for (zone = 0; zone < MAX_NR_ZONES; zone++) {
			enum lru_list lru = type * LRU_INACTIVE_FILE;
			long delta = lrugen->nr_pages[prev][type][zone] -
				     lrugen->nr_pages[next][type][zone];

			if (!delta)
				continue;

			__update_lru_size(lruvec, lru, zone, delta);
			__update_lru_size(lruvec, lru + LRU_ACTIVE, zone, -delta);
		}
	}

	for (type = 0; type < ANON_AND_FILE; type++)
		reset_ctrl_pos(lruvec, type, false);

	WRITE_ONCE(lrugen->timestamps[next], jiffies);
	/* make sure preceding modifications appear */
	smp_store_release(&lrugen->max_seq, lrugen->max_seq + 1);

	spin_unlock_irq(&lruvec_zone(lruvec)->lru_lock);

}

static bool try_to_inc_max_seq(struct lruvec *lruvec, unsigned long max_seq,
			       struct scan_control *sc, bool can_swap, bool full_scan)
{
	bool success;
	struct lru_gen_mm_walk *walk;
	struct mm_struct *mm = NULL;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	VM_WARN_ON_ONCE(max_seq > READ_ONCE(lrugen->max_seq));

	/* see the comment in iterate_mm_list() */
	if (max_seq <= READ_ONCE(lruvec->mm_state.seq)) {
		success = false;
		goto done;
	}

	/*
	 * If the hardware doesn't automatically set the accessed bit, fallback
	 * to lru_gen_look_around(), which only clears the accessed bit in a
	 * handful of PTEs. Spreading the work out over a period of time usually
	 * is less efficient, but it avoids bursty page faults.
	 */
	if (!full_scan && !(arch_has_hw_pte_young() && get_cap(LRU_GEN_MM_WALK))) {
		success = iterate_mm_list_nowalk(lruvec, max_seq);
		goto done;
	}

	walk = set_mm_walk(NULL);
	if (!walk) {
		success = iterate_mm_list_nowalk(lruvec, max_seq);
		goto done;
	}

	walk->lruvec = lruvec;
	walk->max_seq = max_seq;
	walk->can_swap = can_swap;
	walk->full_scan = full_scan;

	do {
		success = iterate_mm_list(lruvec, walk, &mm);
		if (mm)
			walk_mm(lruvec, mm, walk);
	} while (mm);
done:
	if (success)
		inc_max_seq(lruvec, can_swap, full_scan);

	return success;
}

static bool should_run_aging(struct lruvec *lruvec, unsigned long max_seq, unsigned long *min_seq,
			     struct scan_control *sc, bool can_swap, unsigned long *nr_to_scan)
{
	int gen, type, zone;
	unsigned long old = 0;
	unsigned long young = 0;
	unsigned long total = 0;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);

	for (type = !can_swap; type < ANON_AND_FILE; type++) {
		unsigned long seq;

		for (seq = min_seq[type]; seq <= max_seq; seq++) {
			unsigned long size = 0;

			gen = lru_gen_from_seq(seq);

			for (zone = 0; zone < MAX_NR_ZONES; zone++)
				size += max_t(long, READ_ONCE(lrugen->nr_pages[gen][type][zone]),
						0);

			total += size;
			if (seq == max_seq)
				young += size;
			else if (seq + MIN_NR_GENS == max_seq)
				old += size;
		}
	}

	/* try to scrape all its memory if this memcg was deleted */
	*nr_to_scan = mem_cgroup_online(memcg) ? (total >> sc->priority) : total;

	/*
	 * The aging tries to be lazy to reduce the overhead, while the eviction
	 * stalls when the number of generations reaches MIN_NR_GENS. Hence, the
	 * ideal number of generations is MIN_NR_GENS+1.
	 */
	if (min_seq[!can_swap] + MIN_NR_GENS > max_seq)
		return true;
	if (min_seq[!can_swap] + MIN_NR_GENS < max_seq)
		return false;

	/*
	 * It's also ideal to spread pages out evenly, i.e., 1/(MIN_NR_GENS+1)
	 * of the total number of pages for each generation. A reasonable range
	 * for this average portion is [1/MIN_NR_GENS, 1/(MIN_NR_GENS+2)]. The
	 * aging cares about the upper bound of hot pages, while the eviction
	 * cares about the lower bound of cold pages.
	 */
	if (young * MIN_NR_GENS > total)
		return true;
	if (old * (MIN_NR_GENS + 2) < total)
		return true;

	return false;
}

static bool age_lruvec(struct lruvec *lruvec, struct scan_control *sc, unsigned long min_ttl)
{
	bool need_aging;
	unsigned long nr_to_scan;
	int swappiness = get_swappiness(lruvec, sc);
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	VM_WARN_ON_ONCE(sc->memcg_low_reclaim);

	mem_cgroup_calculate_protection(NULL, memcg);

	if (mem_cgroup_below_min(memcg))
		return false;

	need_aging = should_run_aging(lruvec, max_seq, min_seq, sc, swappiness, &nr_to_scan);

	if (min_ttl) {
		int gen = lru_gen_from_seq(min_seq[LRU_GEN_FILE]);
		unsigned long birth = READ_ONCE(lruvec->lrugen.timestamps[gen]);

		if (time_is_after_jiffies(birth + min_ttl))
			return false;

		/* the size is likely too small to be helpful */
		if (!nr_to_scan && sc->priority != DEF_PRIORITY)
			return false;
	}

	if (need_aging)
		try_to_inc_max_seq(lruvec, max_seq, sc, swappiness, false);

	return true;
}

/* to protect the working set of the last N jiffies */
static unsigned long lru_gen_min_ttl __read_mostly;

static void lru_gen_age_node(struct pglist_data *pgdat, struct scan_control *sc)
{
	struct mem_cgroup *memcg;
	bool success = false;
	unsigned long min_ttl = READ_ONCE(lru_gen_min_ttl);

	VM_WARN_ON_ONCE(!current_is_kswapd());

	sc->last_reclaimed = sc->nr_reclaimed;

	/*
	 * To reduce the chance of going into the aging path, which can be
	 * costly, optimistically skip it if the flag below was cleared in the
	 * eviction path. This improves the overall performance when multiple
	 * memcgs are available.
	 */
	if (!sc->memcgs_need_aging) {
		sc->memcgs_need_aging = true;
		return;
	}

	set_mm_walk(pgdat);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);

		if (age_lruvec(lruvec, sc, min_ttl))
			success = true;

		cond_resched();
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));

	clear_mm_walk();

	/* check the order to exclude compaction-induced reclaim */
	if (success || !min_ttl || sc->order)
		return;

	/*
	 * The main goal is to OOM kill if every generation from all memcgs is
	 * younger than min_ttl. However, another possibility is all memcgs are
	 * either below min or empty.
	 */
	if (mutex_trylock(&oom_lock)) {
		struct oom_control oc = {
			.gfp_mask = sc->gfp_mask,
		};

		out_of_memory(&oc);

		mutex_unlock(&oom_lock);
	}
}

/*
 * This function exploits spatial locality when shrink_page_list() walks the
 * rmap. It scans the adjacent PTEs of a young PTE and promotes hot pages. If
 * the scan was done cacheline efficiently, it adds the PMD entry pointing to
 * the PTE table to the Bloom filter. This forms a feedback loop between the
 * eviction and the aging.
 */
void lru_gen_look_around(struct page_vma_mapped_walk *pvmw)
{
	int i;
	pte_t *pte;
	unsigned long start;
	unsigned long end;
	unsigned long addr;
	struct lru_gen_mm_walk *walk;
	int young = 0;
	unsigned long bitmap[BITS_TO_LONGS(MIN_LRU_BATCH)] = {};
	struct page *page = pvmw->page;
	bool can_swap = !page_is_file_cache(page);
	struct mem_cgroup *memcg = page_memcg(page);
	struct pglist_data *pgdat = page_pgdat(page);
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
	DEFINE_MAX_SEQ(lruvec);
	int old_gen, new_gen = lru_gen_from_seq(max_seq);

	lockdep_assert_held(pvmw->ptl);
	VM_WARN_ON_ONCE_PAGE(PageLRU(page), page);

	if (spin_is_contended(pvmw->ptl))
		return;

	/* avoid taking the LRU lock under the PTL when possible */
	walk = current->reclaim_state ? current->reclaim_state->mm_walk : NULL;

	start = max(pvmw->address & PMD_MASK, pvmw->vma->vm_start);
	end = min(pvmw->address | ~PMD_MASK, pvmw->vma->vm_end - 1) + 1;

	if (end - start > MIN_LRU_BATCH * PAGE_SIZE) {
		if (pvmw->address - start < MIN_LRU_BATCH * PAGE_SIZE / 2)
			end = start + MIN_LRU_BATCH * PAGE_SIZE;
		else if (end - pvmw->address < MIN_LRU_BATCH * PAGE_SIZE / 2)
			start = end - MIN_LRU_BATCH * PAGE_SIZE;
		else {
			start = pvmw->address - MIN_LRU_BATCH * PAGE_SIZE / 2;
			end = pvmw->address + MIN_LRU_BATCH * PAGE_SIZE / 2;
		}
	}

	pte = pvmw->pte - (pvmw->address - start) / PAGE_SIZE;

	rcu_read_lock();
	arch_enter_lazy_mmu_mode();

	for (i = 0, addr = start; addr != end; i++, addr += PAGE_SIZE) {
		unsigned long pfn;

		pfn = get_pte_pfn(pte[i], pvmw->vma, addr);
		if (pfn == -1)
			continue;

		if (!pte_young(pte[i]))
			continue;

		page = get_pfn_page(pfn, memcg, pgdat, can_swap);
		if (!page)
			continue;

		if (!ptep_test_and_clear_young(pvmw->vma, addr, pte + i))
			VM_WARN_ON_ONCE(true);

		young++;

		if (pte_dirty(pte[i]) && !PageDirty(page) &&
		    !(PageAnon(page) && PageSwapBacked(page) &&
		      !PageSwapCache(page)))
			set_page_dirty(page);

		old_gen = page_lru_gen(page);
		if (old_gen < 0)
			SetPageReferenced(page);
		else if (old_gen != new_gen)
			__set_bit(i, bitmap);
	}

	arch_leave_lazy_mmu_mode();
	rcu_read_unlock();

	/* feedback from rmap walkers to page table walkers */
	if (suitable_to_scan(i, young))
		update_bloom_filter(lruvec, max_seq, pvmw->pmd);

	if (!walk && bitmap_weight(bitmap, MIN_LRU_BATCH) < PAGEVEC_SIZE) {
		for_each_set_bit(i, bitmap, MIN_LRU_BATCH) {
			page = pte_page(pte[i]);
			activate_page(page);
		}
		return;
	}

	/* page_update_gen() requires stable page_memcg() */
	if (!mem_cgroup_trylock_pages(memcg))
		return;

	if (!walk) {
		spin_lock_irq(&lruvec_zone(lruvec)->lru_lock);
		new_gen = lru_gen_from_seq(lruvec->lrugen.max_seq);
	}

	for_each_set_bit(i, bitmap, MIN_LRU_BATCH) {
		page = compound_head(pte_page(pte[i]));
		if (page_memcg_rcu(page) != memcg)
			continue;

		old_gen = page_update_gen(page, new_gen);
		if (old_gen < 0 || old_gen == new_gen)
			continue;

		if (walk)
			update_batch_size(walk, page, old_gen, new_gen);
		else
			lru_gen_update_size(lruvec, page, old_gen, new_gen);
	}

	if (!walk)
		spin_unlock_irq(&lruvec_zone(lruvec)->lru_lock);

	mem_cgroup_unlock_pages();
}

/******************************************************************************
 *                          the eviction
 ******************************************************************************/

static bool sort_page(struct lruvec *lruvec, struct page *page, struct scan_control *sc,
		       int tier_idx)
{
	bool success;
	int gen = page_lru_gen(page);
	int type = page_is_file_cache(page);
	int zone = page_zonenum(page);
	int delta = hpage_nr_pages(page);
	int refs = page_lru_refs(page);
	int tier = lru_tier_from_refs(refs);
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	VM_WARN_ON_ONCE_PAGE(gen >= MAX_NR_GENS, page);

	/* unevictable */
	if (!page_evictable(page)) {
		success = lru_gen_del_page(lruvec, page, true);
		VM_WARN_ON_ONCE_PAGE(!success, page);
		SetPageUnevictable(page);
		add_page_to_lru_list(page, lruvec, page_lru(page));
		__count_vm_events(UNEVICTABLE_PGCULLED, delta);
		return true;
	}

	/* dirty lazyfree */
	if (type == LRU_GEN_FILE && PageAnon(page) && PageDirty(page)) {
		success = lru_gen_del_page(lruvec, page, true);
		VM_WARN_ON_ONCE_PAGE(!success, page);
		SetPageSwapBacked(page);
		add_page_to_lru_list_tail(page, lruvec, page_lru(page));
		return true;
	}

	/* promoted */
	if (gen != lru_gen_from_seq(lrugen->min_seq[type])) {
		list_move(&page->lru, &lrugen->lists[gen][type][zone]);
		return true;
	}

	/* protected */
	if (tier > tier_idx) {
		int hist = lru_hist_from_seq(lrugen->min_seq[type]);

		gen = page_inc_gen(lruvec, page, false);
		list_move_tail(&page->lru, &lrugen->lists[gen][type][zone]);

		WRITE_ONCE(lrugen->protected[hist][type][tier - 1],
			   lrugen->protected[hist][type][tier - 1] + delta);
		return true;
	}

	/* ineligible */
	/* 4.4: reclaim_idx is always MAX_NR_ZONES - 1 here */
	if (skip_cma(page, sc)) {
		gen = page_inc_gen(lruvec, page, false);
		list_move_tail(&page->lru, &lrugen->lists[gen][type][zone]);
		return true;
	}

	/* waiting for writeback */
	if (PageLocked(page) || PageWriteback(page) ||
	    (type == LRU_GEN_FILE && PageDirty(page))) {
		gen = page_inc_gen(lruvec, page, true);
		list_move(&page->lru, &lrugen->lists[gen][type][zone]);
		return true;
	}

	return false;
}

bool isolate_page(struct lruvec *lruvec, struct page *page, struct scan_control *sc)
{
	bool success;

	/* unmapping inhibited */
	if (!sc->may_unmap && page_mapped(page))
		return false;

	/* swapping inhibited */
	if (!(sc->may_writepage && (sc->gfp_mask & __GFP_IO)) &&
	    (PageDirty(page) ||
	     (PageAnon(page) && !PageSwapCache(page))))
		return false;

	/* raced with release_pages() */
	if (!get_page_unless_zero(page))
		return false;

	/* raced with another isolation */
	if (!TestClearPageLRU(page)) {
		put_page(page);
		return false;
	}

	/* see the comment on MAX_NR_TIERS */
	if (!PageReferenced(page))
		set_mask_bits(&page->flags, LRU_REFS_MASK | LRU_REFS_FLAGS, 0);

	/* for shrink_page_list() */
	ClearPageReclaim(page);
	ClearPageReferenced(page);

	success = lru_gen_del_page(lruvec, page, true);
	VM_WARN_ON_ONCE_PAGE(!success, page);

	return true;
}
EXPORT_SYMBOL_GPL(isolate_page);

static int scan_pages(struct lruvec *lruvec, struct scan_control *sc,
		      int type, int tier, struct list_head *list)
{
	int i;
	int gen;
	enum vm_event_item item;
	int sorted = 0;
	int scanned = 0;
	int isolated = 0;
	int skipped = 0;
	int remaining = MAX_LRU_BATCH;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);

	VM_WARN_ON_ONCE(!list_empty(list));

	if (get_nr_gens(lruvec, type) == MIN_NR_GENS)
		return 0;

	gen = lru_gen_from_seq(lrugen->min_seq[type]);

	for (i = MAX_NR_ZONES; i > 0; i--) {
		LIST_HEAD(moved);
		int skipped_zone = 0;
		/* (MAX_NR_ZONES - 1 + i) % MAX_NR_ZONES, for reclaim_idx = MAX - 1 */
		int zone = i - 1;
		struct list_head *head = &lrugen->lists[gen][type][zone];

		while (!list_empty(head)) {
			struct page *page = lru_to_page(head);
			int delta = hpage_nr_pages(page);

			VM_WARN_ON_ONCE_PAGE(PageUnevictable(page), page);
			VM_WARN_ON_ONCE_PAGE(PageActive(page), page);
			VM_WARN_ON_ONCE_PAGE(page_is_file_cache(page) != type, page);
			VM_WARN_ON_ONCE_PAGE(page_zonenum(page) != zone, page);

			scanned += delta;

			if (sort_page(lruvec, page, sc, tier))
				sorted += delta;
			else if (isolate_page(lruvec, page, sc)) {
				list_add(&page->lru, list);
				isolated += delta;
			} else {
				list_move(&page->lru, &moved);
				skipped_zone += delta;
			}

			if (!--remaining || max(isolated, skipped_zone) >= MIN_LRU_BATCH)
				break;
		}

		if (skipped_zone) {
			list_splice(&moved, head);
			skipped += skipped_zone;
		}

		if (!remaining || isolated >= MIN_LRU_BATCH)
			break;
	}

	/* 4.4: per-zone vm events (NORMAL is the only populated zone);
	 * there is no per-type PGSCAN_ANON counter here.
	 */
	item = current_is_kswapd() ? PGSCAN_KSWAPD_NORMAL : PGSCAN_DIRECT_NORMAL;
	if (!cgroup_reclaim(sc)) {
		__count_vm_events(item, isolated);
		__count_vm_events(PGREFILL_NORMAL, sorted);
	}
	__count_memcg_events(memcg, item, isolated);
	__count_memcg_events(memcg, PGREFILL_NORMAL, sorted);
	trace_mm_vmscan_lru_isolate(sc->order, MAX_LRU_BATCH,
				    scanned, isolated,
				    sc->may_unmap ? 0 : ISOLATE_UNMAPPED,
				    type ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON);

	/*
	 * There might not be eligible pages due to reclaim_idx, may_unmap and
	 * may_writepage. Check the remaining to prevent livelock if it's not
	 * making progress.
	 */
	return isolated || !remaining ? scanned : 0;
}

static int get_tier_idx(struct lruvec *lruvec, int type)
{
	int tier;
	struct ctrl_pos sp, pv;

	/*
	 * To leave a margin for fluctuations, use a larger gain factor (1:2).
	 * This value is chosen because any other tier would have at least twice
	 * as many refaults as the first tier.
	 */
	read_ctrl_pos(lruvec, type, 0, 1, &sp);
	for (tier = 1; tier < MAX_NR_TIERS; tier++) {
		read_ctrl_pos(lruvec, type, tier, 2, &pv);
		if (!positive_ctrl_err(&sp, &pv))
			break;
	}

	return tier - 1;
}

static int get_type_to_scan(struct lruvec *lruvec, int swappiness, int *tier_idx)
{
	int type, tier;
	struct ctrl_pos sp, pv;
	int gain[ANON_AND_FILE] = { swappiness, 200 - swappiness };

	/*
	 * Compare the first tier of anon with that of file to determine which
	 * type to scan. Also need to compare other tiers of the selected type
	 * with the first tier of the other type to determine the last tier (of
	 * the selected type) to evict.
	 */
	read_ctrl_pos(lruvec, LRU_GEN_ANON, 0, gain[LRU_GEN_ANON], &sp);
	read_ctrl_pos(lruvec, LRU_GEN_FILE, 0, gain[LRU_GEN_FILE], &pv);
	type = positive_ctrl_err(&sp, &pv);

	read_ctrl_pos(lruvec, !type, 0, gain[!type], &sp);
	for (tier = 1; tier < MAX_NR_TIERS; tier++) {
		read_ctrl_pos(lruvec, type, tier, gain[type], &pv);
		if (!positive_ctrl_err(&sp, &pv))
			break;
	}

	*tier_idx = tier - 1;

	return type;
}

static int isolate_pages(struct lruvec *lruvec, struct scan_control *sc, int swappiness,
			 int *type_scanned, struct list_head *list)
{
	int i;
	int type;
	int scanned;
	int tier = -1;
	DEFINE_MIN_SEQ(lruvec);

	/*
	 * Try to make the obvious choice first. When anon and file are both
	 * available from the same generation, interpret swappiness 1 as file
	 * first and 200 as anon first.
	 */
	if (!swappiness)
		type = LRU_GEN_FILE;
	else if (min_seq[LRU_GEN_ANON] < min_seq[LRU_GEN_FILE])
		type = LRU_GEN_ANON;
	else if (swappiness == 1)
		type = LRU_GEN_FILE;
	else if (swappiness == 200)
		type = LRU_GEN_ANON;
	else
		type = get_type_to_scan(lruvec, swappiness, &tier);

	for (i = !swappiness; i < ANON_AND_FILE; i++) {
		if (tier < 0)
			tier = get_tier_idx(lruvec, type);

		scanned = scan_pages(lruvec, sc, type, tier, list);
		if (scanned)
			break;

		type = !type;
		tier = -1;
	}

	*type_scanned = type;

	return scanned;
}

static int evict_pages(struct lruvec *lruvec, struct scan_control *sc, int swappiness,
		       bool *need_swapping)
{
	int type;
	int scanned;
	int reclaimed;
	LIST_HEAD(list);
	LIST_HEAD(clean);
	struct page *page;
	struct page *next;
	enum vm_event_item item;
	unsigned long nr_dirty;
	unsigned long nr_unqueued_dirty;
	unsigned long nr_congested;
	unsigned long nr_writeback;
	unsigned long nr_immediate;
	struct lru_gen_mm_walk *walk;
	bool skip_retry = false;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	spin_lock_irq(&lruvec_zone(lruvec)->lru_lock);

	scanned = isolate_pages(lruvec, sc, swappiness, &type, &list);

	scanned += try_to_inc_min_seq(lruvec, swappiness);

	if (get_nr_gens(lruvec, !swappiness) == MIN_NR_GENS)
		scanned = 0;

	spin_unlock_irq(&lruvec_zone(lruvec)->lru_lock);

	if (list_empty(&list))
		return scanned;
retry:
	reclaimed = shrink_page_list(&list, lruvec_zone(lruvec), sc, TTU_UNMAP,
				 &nr_dirty, &nr_unqueued_dirty, &nr_congested,
				 &nr_writeback, &nr_immediate, false);
	sc->nr_reclaimed += reclaimed;
	trace_mm_vmscan_lru_shrink_inactive(pgdat->node_id,
			zone_idx(lruvec_zone(lruvec)),
			scanned, reclaimed,
			sc->priority,
			trace_shrink_flags(type));

	list_for_each_entry_safe_reverse(page, next, &list, lru) {
		bool bypass = false;

		if (bypass)
			continue;

		if (!page_evictable(page)) {
			list_del(&page->lru);
			putback_lru_page(page);
			continue;
		}

		if (PageReclaim(page) &&
		    (PageDirty(page) || PageWriteback(page))) {
			/* restore LRU_REFS_FLAGS cleared by isolate_page() */
			if (PageWorkingset(page))
				SetPageReferenced(page);
			continue;
		}

		if (skip_retry || PageActive(page) || PageReferenced(page) ||
		    page_mapped(page) || PageLocked(page) ||
		    PageDirty(page) || PageWriteback(page)) {
			/* don't add rejected pages to the oldest generation */
			set_mask_bits(&page->flags, LRU_REFS_MASK | LRU_REFS_FLAGS,
				      BIT(PG_active));
			continue;
		}

		/* retry pages that may have missed rotate_reclaimable_page() */
		list_move(&page->lru, &clean);
		sc->nr_scanned -= hpage_nr_pages(page);
	}

	spin_lock_irq(&lruvec_zone(lruvec)->lru_lock);

	putback_inactive_pages(lruvec, &list);

	walk = current->reclaim_state->mm_walk;
	if (walk && walk->batched)
		reset_batch_size(lruvec, walk);

	/* 4.4: per-zone vm events; no per-type PGSTEAL_ANON counter */
	item = current_is_kswapd() ? PGSTEAL_KSWAPD_NORMAL : PGSTEAL_DIRECT_NORMAL;
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, reclaimed);
	__count_memcg_events(memcg, item, reclaimed);

	spin_unlock_irq(&lruvec_zone(lruvec)->lru_lock);

	mem_cgroup_uncharge_list(&list);
	free_hot_cold_page_list(&list, true);

	INIT_LIST_HEAD(&list);
	list_splice_init(&clean, &list);

	if (!list_empty(&list)) {
		skip_retry = true;
		goto retry;
	}

	if (need_swapping && type == LRU_GEN_ANON)
		*need_swapping = true;

	return scanned;
}

/*
 * For future optimizations:
 * 1. Defer try_to_inc_max_seq() to workqueues to reduce latency for memcg
 *    reclaim.
 */
static unsigned long get_nr_to_scan(struct lruvec *lruvec, struct scan_control *sc,
				    bool can_swap, bool *need_aging)
{
	unsigned long nr_to_scan;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	if (mem_cgroup_below_min(memcg) ||
	    (mem_cgroup_below_low(memcg) && !sc->memcg_low_reclaim))
		return 0;

	*need_aging = should_run_aging(lruvec, max_seq, min_seq, sc, can_swap, &nr_to_scan);
	if (!*need_aging)
		return nr_to_scan;

	/* skip the aging path at the default priority */
	if (sc->priority == DEF_PRIORITY)
		goto done;

	/* leave the work to lru_gen_age_node() */
	if (current_is_kswapd())
		return 0;

	if (try_to_inc_max_seq(lruvec, max_seq, sc, can_swap, false))
		return nr_to_scan;
done:
	return min_seq[!can_swap] + MIN_NR_GENS <= max_seq ? nr_to_scan : 0;
}

static bool should_abort_scan(struct lruvec *lruvec, unsigned long seq,
			      struct scan_control *sc, bool need_swapping)
{
	int i;
	DEFINE_MAX_SEQ(lruvec);

	if (!current_is_kswapd()) {
		/* age each memcg at most once to ensure fairness */
		if (max_seq - seq > 1)
			return true;

		/* over-swapping can increase allocation latency */
		if (sc->nr_reclaimed >= sc->nr_to_reclaim && need_swapping)
			return true;

		/* give this thread a chance to exit and free its memory */
		if (fatal_signal_pending(current)) {
			sc->nr_reclaimed += MIN_LRU_BATCH;
			return true;
		}

		if (cgroup_reclaim(sc))
			return false;
	} else if (sc->nr_reclaimed - sc->last_reclaimed < sc->nr_to_reclaim)
		return false;

	/* keep scanning at low priorities to ensure fairness */
	if (sc->priority > DEF_PRIORITY - 2)
		return false;

	/*
	 * A minimum amount of work was done under global memory pressure. For
	 * kswapd, it may be overshooting. For direct reclaim, the allocation
	 * may succeed if all suitable zones are somewhat safe. In either case,
	 * it's better to stop now, and restart later if necessary.
	 */
	/* 4.4: reclaim_idx is MAX_NR_ZONES - 1; all zones are considered */
	for (i = 0; i < MAX_NR_ZONES; i++) {
		unsigned long wmark;
		struct zone *zone = lruvec_pgdat(lruvec)->node_zones + i;

		if (!managed_zone(zone))
			continue;

		wmark = current_is_kswapd() ? high_wmark_pages(zone) : low_wmark_pages(zone);
		if (wmark > zone_page_state(zone, NR_FREE_PAGES))
			return false;
	}

	sc->nr_reclaimed += MIN_LRU_BATCH;

	return true;
}

static void lru_gen_shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
	struct blk_plug plug;
	bool need_aging = false;
	bool need_swapping = false;
	unsigned long scanned = 0;
	unsigned long reclaimed = sc->nr_reclaimed;
	DEFINE_MAX_SEQ(lruvec);

	lru_add_drain();

	blk_start_plug(&plug);

	set_mm_walk(lruvec_pgdat(lruvec));

	while (true) {
		int delta;
		int swappiness;
		unsigned long nr_to_scan;

		if (sc->may_swap)
			swappiness = get_swappiness(lruvec, sc);
		else if (!cgroup_reclaim(sc) && get_swappiness(lruvec, sc))
			swappiness = 1;
		else
			swappiness = 0;

		nr_to_scan = get_nr_to_scan(lruvec, sc, swappiness, &need_aging);
		if (!nr_to_scan)
			goto done;

		delta = evict_pages(lruvec, sc, swappiness, &need_swapping);
		if (!delta)
			goto done;

		scanned += delta;
		if (scanned >= nr_to_scan)
			break;

		if (should_abort_scan(lruvec, max_seq, sc, need_swapping))
			break;

		cond_resched();
	}

	/* see the comment in lru_gen_age_node() */
	if (sc->nr_reclaimed - reclaimed >= MIN_LRU_BATCH && !need_aging)
		sc->memcgs_need_aging = false;
done:
	clear_mm_walk();

	blk_finish_plug(&plug);
}

/******************************************************************************
 *                          state change
 ******************************************************************************/

static bool __maybe_unused state_is_valid(struct lruvec *lruvec)
{
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	if (lrugen->enabled) {
		enum lru_list lru;

		for_each_evictable_lru(lru) {
			if (!list_empty(&lruvec->lists[lru]))
				return false;
		}
	} else {
		int gen, type, zone;

		for_each_gen_type_zone(gen, type, zone) {
			if (!list_empty(&lrugen->lists[gen][type][zone]))
				return false;
		}
	}

	return true;
}

static bool fill_evictable(struct lruvec *lruvec)
{
	enum lru_list lru;
	int remaining = MAX_LRU_BATCH;

	for_each_evictable_lru(lru) {
		int type = is_file_lru(lru);
		bool active = is_active_lru(lru);
		struct list_head *head = &lruvec->lists[lru];

		while (!list_empty(head)) {
			bool success;
			struct page *page = lru_to_page(head);

			VM_WARN_ON_ONCE_PAGE(PageUnevictable(page), page);
			VM_WARN_ON_ONCE_PAGE(PageActive(page) != active, page);
			VM_WARN_ON_ONCE_PAGE(page_is_file_cache(page) != type, page);
			VM_WARN_ON_ONCE_PAGE(page_lru_gen(page) != -1, page);

			del_page_from_lru_list(page, lruvec, page_lru(page));
			success = lru_gen_add_page(lruvec, page, false);
			VM_WARN_ON_ONCE(!success);

			if (!--remaining)
				return false;
		}
	}

	return true;
}

static bool drain_evictable(struct lruvec *lruvec)
{
	int gen, type, zone;
	int remaining = MAX_LRU_BATCH;

	for_each_gen_type_zone(gen, type, zone) {
		struct list_head *head = &lruvec->lrugen.lists[gen][type][zone];

		while (!list_empty(head)) {
			bool success;
			struct page *page = lru_to_page(head);

			VM_WARN_ON_ONCE_PAGE(PageUnevictable(page), page);
			VM_WARN_ON_ONCE_PAGE(PageActive(page), page);
			VM_WARN_ON_ONCE_PAGE(page_is_file_cache(page) != type, page);
			VM_WARN_ON_ONCE_PAGE(page_zonenum(page) != zone, page);

			success = lru_gen_del_page(lruvec, page, false);
			VM_WARN_ON_ONCE(!success);
			add_page_to_lru_list(page, lruvec, page_lru(page));

			if (!--remaining)
				return false;
		}
	}

	return true;
}

static void lru_gen_change_state(bool enabled)
{
	static DEFINE_MUTEX(state_mutex);

	struct mem_cgroup *memcg;

	/* 4.4: cgroup_mutex is static unless CONFIG_PROVE_RCU (exported only
	 * for lockdep); mem_cgroup_iter() pins each memcg via css_tryget_online
	 * and state_mutex serialises the togglers, so the 5.15 tree lock has
	 * no 4.4 equivalent to take here.
	 */
	get_online_cpus();
	get_online_mems();
	mutex_lock(&state_mutex);

	if (enabled == lru_gen_enabled())
		goto unlock;

	if (enabled)
		static_branch_enable(&lru_gen_caps[LRU_GEN_CORE]);
	else
		static_branch_disable(&lru_gen_caps[LRU_GEN_CORE]);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		int nid;

		for_each_node(nid) {
			struct lruvec *lruvec = get_lruvec(memcg, nid);

			if (!lruvec)
				continue;

			spin_lock_irq(&lruvec_zone(lruvec)->lru_lock);

			VM_WARN_ON_ONCE(!seq_is_valid(lruvec));
			VM_WARN_ON_ONCE(!state_is_valid(lruvec));

			lruvec->lrugen.enabled = enabled;

			while (!(enabled ? fill_evictable(lruvec) : drain_evictable(lruvec))) {
				spin_unlock_irq(&lruvec_zone(lruvec)->lru_lock);
				cond_resched();
				spin_lock_irq(&lruvec_zone(lruvec)->lru_lock);
			}

			spin_unlock_irq(&lruvec_zone(lruvec)->lru_lock);
		}

		cond_resched();
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));
unlock:
	mutex_unlock(&state_mutex);
	put_online_mems();
	put_online_cpus();
}

/******************************************************************************
 *                          sysfs interface
 ******************************************************************************/

static ssize_t show_min_ttl(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", jiffies_to_msecs(READ_ONCE(lru_gen_min_ttl)));
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static ssize_t store_min_ttl(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t len)
{
	unsigned int msecs;

	if (kstrtouint(buf, 0, &msecs))
		return -EINVAL;

	WRITE_ONCE(lru_gen_min_ttl, msecs_to_jiffies(msecs));

	return len;
}

static struct kobj_attribute lru_gen_min_ttl_attr = __ATTR(
	min_ttl_ms, 0644, show_min_ttl, store_min_ttl
);

static ssize_t show_enabled(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned int caps = 0;

	if (get_cap(LRU_GEN_CORE))
		caps |= BIT(LRU_GEN_CORE);

	if (arch_has_hw_pte_young() && get_cap(LRU_GEN_MM_WALK))
		caps |= BIT(LRU_GEN_MM_WALK);

	if (IS_ENABLED(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG) && get_cap(LRU_GEN_NONLEAF_YOUNG))
		caps |= BIT(LRU_GEN_NONLEAF_YOUNG);

	return snprintf(buf, PAGE_SIZE, "0x%04x\n", caps);
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static ssize_t store_enabled(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t len)
{
	int i;
	unsigned int caps;

	if (tolower(*buf) == 'n')
		caps = 0;
	else if (tolower(*buf) == 'y')
		caps = -1;
	else if (kstrtouint(buf, 0, &caps))
		return -EINVAL;

	for (i = 0; i < NR_LRU_GEN_CAPS; i++) {
		bool enabled = caps & BIT(i);

		if (i == LRU_GEN_CORE)
			lru_gen_change_state(enabled);
		else if (enabled)
			static_branch_enable(&lru_gen_caps[i]);
		else
			static_branch_disable(&lru_gen_caps[i]);
	}

	return len;
}

static struct kobj_attribute lru_gen_enabled_attr = __ATTR(
	enabled, 0644, show_enabled, store_enabled
);

static struct attribute *lru_gen_attrs[] = {
	&lru_gen_min_ttl_attr.attr,
	&lru_gen_enabled_attr.attr,
	NULL
};

static struct attribute_group lru_gen_attr_group = {
	.name = "lru_gen",
	.attrs = lru_gen_attrs,
};

/******************************************************************************
 *                          debugfs interface
 ******************************************************************************/

static void *lru_gen_seq_start(struct seq_file *m, loff_t *pos)
{
	struct mem_cgroup *memcg;
	loff_t nr_to_skip = *pos;

	m->private = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!m->private)
		return ERR_PTR(-ENOMEM);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		int nid;

		for_each_node_state(nid, N_MEMORY) {
			if (!nr_to_skip--)
				return get_lruvec(memcg, nid);
		}
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));

	return NULL;
}

static void lru_gen_seq_stop(struct seq_file *m, void *v)
{
	if (!IS_ERR_OR_NULL(v))
		mem_cgroup_iter_break(NULL, lruvec_memcg(v));

	kvfree(m->private);
	m->private = NULL;
}

static void *lru_gen_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	int nid = lruvec_pgdat(v)->node_id;
	struct mem_cgroup *memcg = lruvec_memcg(v);

	++*pos;

	nid = next_node(nid, node_states[N_MEMORY]);
	if (nid == MAX_NUMNODES) {
		memcg = mem_cgroup_iter(NULL, memcg, NULL);
		if (!memcg)
			return NULL;

		nid = first_node(node_states[N_MEMORY]);
	}

	return get_lruvec(memcg, nid);
}

static void lru_gen_seq_show_full(struct seq_file *m, struct lruvec *lruvec,
				  unsigned long max_seq, unsigned long *min_seq,
				  unsigned long seq)
{
	int i;
	int type, tier;
	int hist = lru_hist_from_seq(seq);
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	for (tier = 0; tier < MAX_NR_TIERS; tier++) {
		seq_printf(m, "            %10d", tier);
		for (type = 0; type < ANON_AND_FILE; type++) {
			const char *s = "   ";
			unsigned long n[3] = {};

			if (seq == max_seq) {
				s = "RT ";
				n[0] = READ_ONCE(lrugen->avg_refaulted[type][tier]);
				n[1] = READ_ONCE(lrugen->avg_total[type][tier]);
			} else if (seq == min_seq[type] || NR_HIST_GENS > 1) {
				s = "rep";
				n[0] = atomic_long_read(&lrugen->refaulted[hist][type][tier]);
				n[1] = atomic_long_read(&lrugen->evicted[hist][type][tier]);
				if (tier)
					n[2] = READ_ONCE(lrugen->protected[hist][type][tier - 1]);
			}

			for (i = 0; i < 3; i++)
				seq_printf(m, " %10lu%c", n[i], s[i]);
		}
		seq_putc(m, '\n');
	}

	seq_puts(m, "                      ");
	for (i = 0; i < NR_MM_STATS; i++) {
		const char *s = "      ";
		unsigned long n = 0;

		if (seq == max_seq && NR_HIST_GENS == 1) {
			s = "LOYNFA";
			n = READ_ONCE(lruvec->mm_state.stats[hist][i]);
		} else if (seq != max_seq && NR_HIST_GENS > 1) {
			s = "loynfa";
			n = READ_ONCE(lruvec->mm_state.stats[hist][i]);
		}

		seq_printf(m, " %10lu%c", n, s[i]);
	}
	seq_putc(m, '\n');
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static int lru_gen_seq_show(struct seq_file *m, void *v)
{
	unsigned long seq;
	/* 4.4: debugfs does not wrap fops, so file->f_op is the real one */
	bool full = !m->file->f_op->write;
	struct lruvec *lruvec = v;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	int nid = lruvec_pgdat(lruvec)->node_id;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	if (nid == first_node(node_states[N_MEMORY])) {
		const char *path = memcg ? m->private : "";

#ifdef CONFIG_MEMCG
		if (memcg)
			cgroup_path(memcg->css.cgroup, m->private, PATH_MAX);
#endif
		seq_printf(m, "memcg %5hu %s\n", mem_cgroup_id(memcg), path);
	}

	seq_printf(m, " node %5d\n", nid);

	if (!full)
		seq = min_seq[LRU_GEN_ANON];
	else if (max_seq >= MAX_NR_GENS)
		seq = max_seq - MAX_NR_GENS + 1;
	else
		seq = 0;

	for (; seq <= max_seq; seq++) {
		int type, zone;
		int gen = lru_gen_from_seq(seq);
		unsigned long birth = READ_ONCE(lruvec->lrugen.timestamps[gen]);

		seq_printf(m, " %10lu %10u", seq, jiffies_to_msecs(jiffies - birth));

		for (type = 0; type < ANON_AND_FILE; type++) {
			unsigned long size = 0;
			char mark = full && seq < min_seq[type] ? 'x' : ' ';

			for (zone = 0; zone < MAX_NR_ZONES; zone++)
				size += max_t(long, READ_ONCE(lrugen->nr_pages[gen][type][zone]),
						0);

			seq_printf(m, " %10lu%c", size, mark);
		}

		seq_putc(m, '\n');

		if (full)
			lru_gen_seq_show_full(m, lruvec, max_seq, min_seq, seq);
	}

	return 0;
}

static const struct seq_operations lru_gen_seq_ops = {
	.start = lru_gen_seq_start,
	.stop = lru_gen_seq_stop,
	.next = lru_gen_seq_next,
	.show = lru_gen_seq_show,
};

static int run_aging(struct lruvec *lruvec, unsigned long seq, struct scan_control *sc,
		     bool can_swap, bool full_scan)
{
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	if (seq < max_seq)
		return 0;

	if (seq > max_seq)
		return -EINVAL;

	if (!full_scan && min_seq[!can_swap] + MAX_NR_GENS - 1 <= max_seq)
		return -ERANGE;

	try_to_inc_max_seq(lruvec, max_seq, sc, can_swap, full_scan);

	return 0;
}

static int run_eviction(struct lruvec *lruvec, unsigned long seq, struct scan_control *sc,
			int swappiness, unsigned long nr_to_reclaim)
{
	DEFINE_MAX_SEQ(lruvec);

	if (seq + MIN_NR_GENS > max_seq)
		return -EINVAL;

	sc->nr_reclaimed = 0;

	while (!signal_pending(current)) {
		DEFINE_MIN_SEQ(lruvec);

		if (seq < min_seq[!swappiness])
			return 0;

		if (sc->nr_reclaimed >= nr_to_reclaim)
			return 0;

		if (!evict_pages(lruvec, sc, swappiness, NULL))
			return 0;

		cond_resched();
	}

	return -EINTR;
}

static int run_cmd(char cmd, int memcg_id, int nid, unsigned long seq,
		   struct scan_control *sc, int swappiness, unsigned long opt)
{
	struct lruvec *lruvec;
	int err = -EINVAL;
	struct mem_cgroup *memcg = NULL;

	if (nid < 0 || nid >= MAX_NUMNODES || !node_state(nid, N_MEMORY))
		return -EINVAL;

	if (!mem_cgroup_disabled()) {
		rcu_read_lock();
		memcg = mem_cgroup_from_id(memcg_id);
#ifdef CONFIG_MEMCG
		if (memcg && !css_tryget(&memcg->css))
			memcg = NULL;
#endif
		rcu_read_unlock();

		if (!memcg)
			return -EINVAL;
	}

	if (memcg_id != mem_cgroup_id(memcg))
		goto done;

	lruvec = get_lruvec(memcg, nid);

	if (swappiness < 0)
		swappiness = get_swappiness(lruvec, sc);
	else if (swappiness > 200)
		goto done;

	switch (cmd) {
	case '+':
		err = run_aging(lruvec, seq, sc, swappiness, opt);
		break;
	case '-':
		err = run_eviction(lruvec, seq, sc, swappiness, opt);
		break;
	}
done:
	mem_cgroup_put(memcg);

	return err;
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static ssize_t lru_gen_seq_write(struct file *file, const char __user *src,
				 size_t len, loff_t *pos)
{
	void *buf;
	char *cur, *next;
	unsigned int flags;
	struct blk_plug plug;
	int err = -EINVAL;
	struct scan_control sc = {
		.may_writepage = true,
		.may_unmap = true,
		.may_swap = true,
		/* 4.4: no reclaim_idx member; the zonelists bound the zones */
		.gfp_mask = GFP_KERNEL,
	};

	buf = kmalloc(len + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, src, len)) {
		kvfree(buf);
		return -EFAULT;
	}

	set_task_reclaim_state(current, &sc.reclaim_state);
	flags = memalloc_noreclaim_save();
	blk_start_plug(&plug);
	if (!set_mm_walk(NULL)) {
		err = -ENOMEM;
		goto done;
	}

	next = buf;
	next[len] = '\0';

	while ((cur = strsep(&next, ",;\n"))) {
		int n;
		int end;
		char cmd;
		unsigned int memcg_id;
		unsigned int nid;
		unsigned long seq;
		unsigned int swappiness = -1;
		unsigned long opt = -1;

		cur = skip_spaces(cur);
		if (!*cur)
			continue;

		n = sscanf(cur, "%c %u %u %lu %n %u %n %lu %n", &cmd, &memcg_id, &nid,
			   &seq, &end, &swappiness, &end, &opt, &end);
		if (n < 4 || cur[end]) {
			err = -EINVAL;
			break;
		}

		err = run_cmd(cmd, memcg_id, nid, seq, &sc, swappiness, opt);
		if (err)
			break;
	}
done:
	clear_mm_walk();
	blk_finish_plug(&plug);
	memalloc_noreclaim_restore(flags);
	set_task_reclaim_state(current, NULL);

	kvfree(buf);

	return err ? : len;
}

static int lru_gen_seq_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &lru_gen_seq_ops);
}

static const struct file_operations lru_gen_rw_fops = {
	.open = lru_gen_seq_open,
	.read = seq_read,
	.write = lru_gen_seq_write,
	.llseek = seq_lseek,
	.release = seq_release,
};

static const struct file_operations lru_gen_ro_fops = {
	.open = lru_gen_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

/******************************************************************************
 *                          initialization
 ******************************************************************************/

void lru_gen_init_lruvec(struct lruvec *lruvec)
{
	int i;
	int gen, type, zone;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	lrugen->max_seq = MIN_NR_GENS + 1;
	lrugen->enabled = lru_gen_enabled();

	for (i = 0; i <= MIN_NR_GENS + 1; i++)
		lrugen->timestamps[i] = jiffies;

	for_each_gen_type_zone(gen, type, zone)
		INIT_LIST_HEAD(&lrugen->lists[gen][type][zone]);

	lruvec->mm_state.seq = MIN_NR_GENS;
}

#ifdef CONFIG_MEMCG
void lru_gen_init_memcg(struct mem_cgroup *memcg)
{
	INIT_LIST_HEAD(&memcg->mm_list.fifo);
	spin_lock_init(&memcg->mm_list.lock);
}

void lru_gen_exit_memcg(struct mem_cgroup *memcg)
{
	int i;
	int nid;

	for_each_node(nid) {
		struct lruvec *lruvec = get_lruvec(memcg, nid);

		VM_WARN_ON_ONCE(memchr_inv(lruvec->lrugen.nr_pages, 0,
					   sizeof(lruvec->lrugen.nr_pages)));

		for (i = 0; i < NR_BLOOM_FILTERS; i++) {
			bitmap_free(lruvec->mm_state.filters[i]);
			lruvec->mm_state.filters[i] = NULL;
		}
	}
}
#endif

static int __init init_lru_gen(void)
{
	BUILD_BUG_ON(MIN_NR_GENS + 1 >= MAX_NR_GENS);
	BUILD_BUG_ON(BIT(LRU_GEN_WIDTH) <= MAX_NR_GENS);

	if (sysfs_create_group(mm_kobj, &lru_gen_attr_group))
		pr_err("lru_gen: failed to create sysfs group\n");

	debugfs_create_file("lru_gen", 0644, NULL, NULL, &lru_gen_rw_fops);
	debugfs_create_file("lru_gen_full", 0444, NULL, NULL, &lru_gen_ro_fops);

	return 0;
};
late_initcall(init_lru_gen);

#else /* !CONFIG_LRU_GEN */

static void lru_gen_age_node(struct pglist_data *pgdat, struct scan_control *sc)
{
}

static void lru_gen_shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
}

#endif /* CONFIG_LRU_GEN */

static void shrink_lruvec(struct lruvec *lruvec, int swappiness,
			  struct scan_control *sc, unsigned long *lru_pages)
{
	unsigned long nr[NR_LRU_LISTS];
	unsigned long targets[NR_LRU_LISTS];
	unsigned long nr_to_scan;
	enum lru_list lru;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;
	struct blk_plug plug;
	bool scan_adjusted;

	if (lru_gen_enabled()) {
		lru_gen_shrink_lruvec(lruvec, sc);
		return;
	}

	get_scan_count(lruvec, swappiness, sc, nr, lru_pages);

	/* Record the original scan target for proportional adjustments later */
	memcpy(targets, nr, sizeof(nr));

	/*
	 * Global reclaiming within direct reclaim at DEF_PRIORITY is a normal
	 * event that can occur when there is little memory pressure e.g.
	 * multiple streaming readers/writers. Hence, we do not abort scanning
	 * when the requested number of pages are reclaimed when scanning at
	 * DEF_PRIORITY on the assumption that the fact we are direct
	 * reclaiming implies that kswapd is not keeping up and it is best to
	 * do a batch of work at once. For memcg reclaim one check is made to
	 * abort proportional reclaim if either the file or anon lru has already
	 * dropped to zero at the first pass.
	 */
	scan_adjusted = (global_reclaim(sc) && !current_is_kswapd() &&
			 sc->priority == DEF_PRIORITY);

	blk_start_plug(&plug);
	while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_FILE] ||
					nr[LRU_INACTIVE_FILE]) {
		unsigned long nr_anon, nr_file, percentage;
		unsigned long nr_scanned;

		for_each_evictable_lru(lru) {
			if (nr[lru]) {
				nr_to_scan = min(nr[lru], SWAP_CLUSTER_MAX);
				nr[lru] -= nr_to_scan;

				nr_reclaimed += shrink_list(lru, nr_to_scan,
							    lruvec, sc);
			}
		}

		if (nr_reclaimed < nr_to_reclaim || scan_adjusted)
			continue;

		/*
		 * For kswapd and memcg, reclaim at least the number of pages
		 * requested. Ensure that the anon and file LRUs are scanned
		 * proportionally what was requested by get_scan_count(). We
		 * stop reclaiming one LRU and reduce the amount scanning
		 * proportional to the original scan target.
		 */
		nr_file = nr[LRU_INACTIVE_FILE] + nr[LRU_ACTIVE_FILE];
		nr_anon = nr[LRU_INACTIVE_ANON] + nr[LRU_ACTIVE_ANON];

		/*
		 * It's just vindictive to attack the larger once the smaller
		 * has gone to zero.  And given the way we stop scanning the
		 * smaller below, this makes sure that we only make one nudge
		 * towards proportionality once we've got nr_to_reclaim.
		 */
		if (!nr_file || !nr_anon)
			break;

		if (nr_file > nr_anon) {
			unsigned long scan_target = targets[LRU_INACTIVE_ANON] +
						targets[LRU_ACTIVE_ANON] + 1;
			lru = LRU_BASE;
			percentage = nr_anon * 100 / scan_target;
		} else {
			unsigned long scan_target = targets[LRU_INACTIVE_FILE] +
						targets[LRU_ACTIVE_FILE] + 1;
			lru = LRU_FILE;
			percentage = nr_file * 100 / scan_target;
		}

		/* Stop scanning the smaller of the LRU */
		nr[lru] = 0;
		nr[lru + LRU_ACTIVE] = 0;

		/*
		 * Recalculate the other LRU scan count based on its original
		 * scan target and the percentage scanning already complete
		 */
		lru = (lru == LRU_FILE) ? LRU_BASE : LRU_FILE;
		nr_scanned = targets[lru] - nr[lru];
		nr[lru] = targets[lru] * (100 - percentage) / 100;
		nr[lru] -= min(nr[lru], nr_scanned);

		lru += LRU_ACTIVE;
		nr_scanned = targets[lru] - nr[lru];
		nr[lru] = targets[lru] * (100 - percentage) / 100;
		nr[lru] -= min(nr[lru], nr_scanned);

		scan_adjusted = true;
	}
	blk_finish_plug(&plug);
	sc->nr_reclaimed += nr_reclaimed;

	if (need_memory_boosting(NULL, true))
		return;

	/*
	 * Even if we did not try to evict anon pages at all, we want to
	 * rebalance the anon lru active/inactive ratio.
	 */
	if (inactive_anon_is_low(lruvec))
		shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
				   sc, LRU_ACTIVE_ANON);

	throttle_vm_writeout(sc->gfp_mask);
}

/* Use reclaim/compaction for costly allocs or under memory pressure */
static bool in_reclaim_compaction(struct scan_control *sc)
{
	if (IS_ENABLED(CONFIG_COMPACTION) && sc->order &&
			(sc->order > PAGE_ALLOC_COSTLY_ORDER ||
			 sc->priority < DEF_PRIORITY - 2))
		return true;

	return false;
}

/*
 * Reclaim/compaction is used for high-order allocation requests. It reclaims
 * order-0 pages before compacting the zone. should_continue_reclaim() returns
 * true if more pages should be reclaimed such that when the page allocator
 * calls try_to_compact_zone() that it will have enough free pages to succeed.
 * It will give up earlier than that if there is difficulty reclaiming pages.
 */
static inline bool should_continue_reclaim(struct zone *zone,
					unsigned long nr_reclaimed,
					unsigned long nr_scanned,
					struct scan_control *sc)
{
	unsigned long pages_for_compaction;
	unsigned long inactive_lru_pages;

	/* If not in reclaim/compaction mode, stop */
	if (!in_reclaim_compaction(sc))
		return false;

	/* Consider stopping depending on scan and reclaim activity */
	if (sc->gfp_mask & __GFP_REPEAT) {
		/*
		 * For __GFP_REPEAT allocations, stop reclaiming if the
		 * full LRU list has been scanned and we are still failing
		 * to reclaim pages. This full LRU scan is potentially
		 * expensive but a __GFP_REPEAT caller really wants to succeed
		 */
		if (!nr_reclaimed && !nr_scanned)
			return false;
	} else {
		/*
		 * For non-__GFP_REPEAT allocations which can presumably
		 * fail without consequence, stop if we failed to reclaim
		 * any pages from the last SWAP_CLUSTER_MAX number of
		 * pages that were scanned. This will return to the
		 * caller faster at the risk reclaim/compaction and
		 * the resulting allocation attempt fails
		 */
		if (!nr_reclaimed)
			return false;
	}

	/*
	 * If we have not reclaimed enough pages for compaction and the
	 * inactive lists are large enough, continue reclaiming
	 */
	pages_for_compaction = (2UL << sc->order);
	inactive_lru_pages = zone_page_state(zone, NR_INACTIVE_FILE);
	if (get_nr_swap_pages() > 0)
		inactive_lru_pages += zone_page_state(zone, NR_INACTIVE_ANON);
	if (sc->nr_reclaimed < pages_for_compaction &&
			inactive_lru_pages > pages_for_compaction)
		return true;

	/* If compaction would go ahead or the allocation would succeed, stop */
	switch (compaction_suitable(zone, sc->order, 0, 0)) {
	case COMPACT_PARTIAL:
	case COMPACT_CONTINUE:
		return false;
	default:
		return true;
	}
}

static bool shrink_zone(struct zone *zone, struct scan_control *sc,
			bool is_classzone)
{
	struct reclaim_state *reclaim_state = current->reclaim_state;
	unsigned long nr_reclaimed, nr_scanned;
	bool reclaimable = false;

	do {
		struct mem_cgroup *root = sc->target_mem_cgroup;
		struct mem_cgroup_reclaim_cookie reclaim = {
			.zone = zone,
			.priority = sc->priority,
		};
		unsigned long zone_lru_pages = 0;
		struct mem_cgroup *memcg;

		nr_reclaimed = sc->nr_reclaimed;
		nr_scanned = sc->nr_scanned;

		memcg = mem_cgroup_iter(root, NULL, &reclaim);
		do {
			unsigned long lru_pages;
			unsigned long scanned;
			struct lruvec *lruvec;
			int swappiness;

			if (mem_cgroup_low(root, memcg)) {
				if (!sc->may_thrash)
					continue;
				mem_cgroup_events(memcg, MEMCG_LOW, 1);
			}

			lruvec = mem_cgroup_zone_lruvec(zone, memcg);
			swappiness = mem_cgroup_swappiness(memcg);
			scanned = sc->nr_scanned;

			shrink_lruvec(lruvec, swappiness, sc, &lru_pages);
			zone_lru_pages += lru_pages;

			if (memcg && is_classzone)
				shrink_slab(sc->gfp_mask, zone_to_nid(zone),
					    memcg, sc->nr_scanned - scanned,
					    lru_pages);

			/*
			 * Direct reclaim and kswapd have to scan all memory
			 * cgroups to fulfill the overall scan target for the
			 * zone.
			 *
			 * Limit reclaim, on the other hand, only cares about
			 * nr_to_reclaim pages to be reclaimed and it will
			 * retry with decreasing priority if one round over the
			 * whole hierarchy is not sufficient.
			 */
			if (!global_reclaim(sc) &&
					sc->nr_reclaimed >= sc->nr_to_reclaim) {
				mem_cgroup_iter_break(root, memcg);
				break;
			}
		} while ((memcg = mem_cgroup_iter(root, memcg, &reclaim)));

		/*
		 * Shrink the slab caches in the same proportion that
		 * the eligible LRU pages were scanned.
		 */
		if (global_reclaim(sc) && is_classzone)
			shrink_slab(sc->gfp_mask, zone_to_nid(zone), NULL,
				    sc->nr_scanned - nr_scanned,
				    zone_lru_pages);

		if (reclaim_state) {
			sc->nr_reclaimed += reclaim_state->reclaimed_slab;
			reclaim_state->reclaimed_slab = 0;
		}

		vmpressure(sc->gfp_mask, sc->target_mem_cgroup,
			   sc->nr_scanned - nr_scanned,
			   sc->nr_reclaimed - nr_reclaimed);

		if (sc->nr_reclaimed - nr_reclaimed)
			reclaimable = true;

	} while (should_continue_reclaim(zone, sc->nr_reclaimed - nr_reclaimed,
					 sc->nr_scanned - nr_scanned, sc));

	return reclaimable;
}

/*
 * Returns true if compaction should go ahead for a high-order request, or
 * the high-order allocation would succeed without compaction.
 */
static inline bool compaction_ready(struct zone *zone, int order)
{
	unsigned long balance_gap, watermark;
	bool watermark_ok;

	/*
	 * Compaction takes time to run and there are potentially other
	 * callers using the pages just freed. Continue reclaiming until
	 * there is a buffer of free pages available to give compaction
	 * a reasonable chance of completing and allocating the page
	 */
	balance_gap = min(low_wmark_pages(zone), DIV_ROUND_UP(
			zone->managed_pages, KSWAPD_ZONE_BALANCE_GAP_RATIO));
	watermark = high_wmark_pages(zone) + balance_gap + (2UL << order);
	watermark_ok = zone_watermark_ok_safe(zone, 0, watermark, 0);

	/*
	 * If compaction is deferred, reclaim up to a point where
	 * compaction will have a chance of success when re-enabled
	 */
	if (compaction_deferred(zone, order))
		return watermark_ok;

	/*
	 * If compaction is not ready to start and allocation is not likely
	 * to succeed without it, then keep reclaiming.
	 */
	if (compaction_suitable(zone, order, 0, 0) == COMPACT_SKIPPED)
		return false;

	return watermark_ok;
}

/*
 * This is the direct reclaim path, for page-allocating processes.  We only
 * try to reclaim pages from zones which will satisfy the caller's allocation
 * request.
 *
 * We reclaim from a zone even if that zone is over high_wmark_pages(zone).
 * Because:
 * a) The caller may be trying to free *extra* pages to satisfy a higher-order
 *    allocation or
 * b) The target zone may be at high_wmark_pages(zone) but the lower zones
 *    must go *over* high_wmark_pages(zone) to satisfy the `incremental min'
 *    zone defense algorithm.
 *
 * If a zone is deemed to be full of pinned pages then just give it a light
 * scan then give up on it.
 *
 * Returns true if a zone was reclaimable.
 */
static bool shrink_zones(struct zonelist *zonelist, struct scan_control *sc)
{
	struct zoneref *z;
	struct zone *zone;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	gfp_t orig_mask;
	enum zone_type requested_highidx = gfp_zone(sc->gfp_mask);
	bool reclaimable = false;

	/*
	 * If the number of buffer_heads in the machine exceeds the maximum
	 * allowed level, force direct reclaim to scan the highmem zone as
	 * highmem pages could be pinning lowmem pages storing buffer_heads
	 */
	orig_mask = sc->gfp_mask;
	if (buffer_heads_over_limit)
		sc->gfp_mask |= __GFP_HIGHMEM;

	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					gfp_zone(sc->gfp_mask), sc->nodemask) {
		enum zone_type classzone_idx;

		if (!populated_zone(zone))
			continue;

		classzone_idx = gfp_zone(sc->gfp_mask);
		while (!populated_zone(zone->zone_pgdat->node_zones +
							classzone_idx))
			classzone_idx--;

		/*
		 * Take care memory controller reclaiming has small influence
		 * to global LRU.
		 */
		if (global_reclaim(sc)) {
			if (!cpuset_zone_allowed(zone,
						 GFP_KERNEL | __GFP_HARDWALL))
				continue;

			if (sc->priority != DEF_PRIORITY &&
			    !zone_reclaimable(zone))
				continue;	/* Let kswapd poll it */

			/*
			 * If we already have plenty of memory free for
			 * compaction in this zone, don't free any more.
			 * Even though compaction is invoked for any
			 * non-zero order, only frequent costly order
			 * reclamation is disruptive enough to become a
			 * noticeable problem, like transparent huge
			 * page allocations.
			 */
			if (IS_ENABLED(CONFIG_COMPACTION) &&
			    sc->order > PAGE_ALLOC_COSTLY_ORDER &&
			    zonelist_zone_idx(z) <= requested_highidx &&
			    compaction_ready(zone, sc->order)) {
				sc->compaction_ready = true;
				continue;
			}

			/*
			 * This steals pages from memory cgroups over softlimit
			 * and returns the number of reclaimed pages and
			 * scanned pages. This works for global memory pressure
			 * and balancing, not for a memcg's limit.
			 */
			nr_soft_scanned = 0;
			nr_soft_reclaimed = mem_cgroup_soft_limit_reclaim(zone,
						sc->order, sc->gfp_mask,
						&nr_soft_scanned);
			sc->nr_reclaimed += nr_soft_reclaimed;
			sc->nr_scanned += nr_soft_scanned;
			if (nr_soft_reclaimed)
				reclaimable = true;
			/* need some check for avoid more shrink_zone() */
		}

		if (shrink_zone(zone, sc, zone_idx(zone) == classzone_idx))
			reclaimable = true;

		if (global_reclaim(sc) &&
		    !reclaimable && zone_reclaimable(zone))
			reclaimable = true;
	}

	/*
	 * Restore to original mask to avoid the impact on the caller if we
	 * promoted it to __GFP_HIGHMEM.
	 */
	sc->gfp_mask = orig_mask;

	return reclaimable;
}

/*
 * This is the main entry point to direct page reclaim.
 *
 * If a full scan of the inactive list fails to free enough memory then we
 * are "out of memory" and something needs to be killed.
 *
 * If the caller is !__GFP_FS then the probability of a failure is reasonably
 * high - the zone may be full of dirty or under-writeback pages, which this
 * caller can't do much about.  We kick the writeback threads and take explicit
 * naps in the hope that some of these pages can be written.  But if the
 * allocating task holds filesystem locks which prevent writeout this might not
 * work, and the allocation attempt will fail.
 *
 * returns:	0, if no pages reclaimed
 * 		else, the number of pages reclaimed
 */
static unsigned long do_try_to_free_pages(struct zonelist *zonelist,
					  struct scan_control *sc)
{
	int initial_priority = sc->priority;
	unsigned long total_scanned = 0;
	unsigned long pflags;
	unsigned long writeback_threshold;
	bool zones_reclaimable;
retry:
	delayacct_freepages_start();

	if (global_reclaim(sc))
		count_vm_event(ALLOCSTALL);

	/* Account the synchronous reclaim as PSI memory stall time. */
	psi_memstall_enter(&pflags);

	do {
		vmpressure_prio(sc->gfp_mask, sc->target_mem_cgroup,
				sc->priority);
		sc->nr_scanned = 0;
		zones_reclaimable = shrink_zones(zonelist, sc);

		total_scanned += sc->nr_scanned;
		if (sc->nr_reclaimed >= sc->nr_to_reclaim)
			break;

		if (sc->compaction_ready)
			break;

		/*
		 * If we're getting trouble reclaiming, start doing
		 * writepage even in laptop mode.
		 */
		if (sc->priority < DEF_PRIORITY - 2)
			sc->may_writepage = 1;

		/*
		 * Try to write back as many pages as we just scanned.  This
		 * tends to cause slow streaming writers to write data to the
		 * disk smoothly, at the dirtying rate, which is nice.   But
		 * that's undesirable in laptop mode, where we *want* lumpy
		 * writeout.  So in laptop mode, write out the whole world.
		 */
		writeback_threshold = sc->nr_to_reclaim + sc->nr_to_reclaim / 2;
		if (total_scanned > writeback_threshold) {
			wakeup_flusher_threads(laptop_mode ? 0 : total_scanned,
						WB_REASON_TRY_TO_FREE_PAGES);
			sc->may_writepage = 1;
		}
	} while (--sc->priority >= 0);

	psi_memstall_leave(&pflags);

	delayacct_freepages_end();

	if (sc->nr_reclaimed)
		return sc->nr_reclaimed;

	/* Aborted reclaim to try compaction? don't OOM, then */
	if (sc->compaction_ready)
		return 1;

	/* Untapped cgroup reserves?  Don't OOM, retry. */
	if (!sc->may_thrash) {
		sc->priority = initial_priority;
		sc->may_thrash = 1;
		goto retry;
	}

	/* Any of the zones still reclaimable?  Don't OOM. */
	if (zones_reclaimable)
		return 1;

	return 0;
}

static bool pfmemalloc_watermark_ok(pg_data_t *pgdat)
{
	struct zone *zone;
	unsigned long pfmemalloc_reserve = 0;
	unsigned long free_pages = 0;
	int i;
	bool wmark_ok;

	for (i = 0; i <= ZONE_NORMAL; i++) {
		zone = &pgdat->node_zones[i];
		if (!populated_zone(zone) ||
		    zone_reclaimable_pages(zone) == 0)
			continue;

		pfmemalloc_reserve += min_wmark_pages(zone);
		free_pages += zone_page_state(zone, NR_FREE_PAGES);
	}

	/* If there are no reserves (unexpected config) then do not throttle */
	if (!pfmemalloc_reserve)
		return true;

	wmark_ok = free_pages > pfmemalloc_reserve / 2;

	/* kswapd must be awake if processes are being throttled */
	if (!wmark_ok && waitqueue_active(&pgdat->kswapd_wait)) {
		pgdat->classzone_idx = min(pgdat->classzone_idx,
						(enum zone_type)ZONE_NORMAL);
		wake_up_interruptible(&pgdat->kswapd_wait);
	}

	return wmark_ok;
}

/*
 * Throttle direct reclaimers if backing storage is backed by the network
 * and the PFMEMALLOC reserve for the preferred node is getting dangerously
 * depleted. kswapd will continue to make progress and wake the processes
 * when the low watermark is reached.
 *
 * Returns true if a fatal signal was delivered during throttling. If this
 * happens, the page allocator should not consider triggering the OOM killer.
 */
static bool throttle_direct_reclaim(gfp_t gfp_mask, struct zonelist *zonelist,
					nodemask_t *nodemask)
{
	struct zoneref *z;
	struct zone *zone;
	pg_data_t *pgdat = NULL;

	/*
	 * Kernel threads should not be throttled as they may be indirectly
	 * responsible for cleaning pages necessary for reclaim to make forward
	 * progress. kjournald for example may enter direct reclaim while
	 * committing a transaction where throttling it could forcing other
	 * processes to block on log_wait_commit().
	 */
	if (current->flags & PF_KTHREAD)
		goto out;

	/*
	 * If a fatal signal is pending, this process should not throttle.
	 * It should return quickly so it can exit and free its memory
	 */
	if (fatal_signal_pending(current))
		goto out;

	/*
	 * Check if the pfmemalloc reserves are ok by finding the first node
	 * with a usable ZONE_NORMAL or lower zone. The expectation is that
	 * GFP_KERNEL will be required for allocating network buffers when
	 * swapping over the network so ZONE_HIGHMEM is unusable.
	 *
	 * Throttling is based on the first usable node and throttled processes
	 * wait on a queue until kswapd makes progress and wakes them. There
	 * is an affinity then between processes waking up and where reclaim
	 * progress has been made assuming the process wakes on the same node.
	 * More importantly, processes running on remote nodes will not compete
	 * for remote pfmemalloc reserves and processes on different nodes
	 * should make reasonable progress.
	 */
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					gfp_zone(gfp_mask), nodemask) {
		if (zone_idx(zone) > ZONE_NORMAL)
			continue;

		/* Throttle based on the first usable node */
		pgdat = zone->zone_pgdat;
		if (pfmemalloc_watermark_ok(pgdat))
			goto out;
		break;
	}

	/* If no zone was usable by the allocation flags then do not throttle */
	if (!pgdat)
		goto out;

	/* Account for the throttling */
	count_vm_event(PGSCAN_DIRECT_THROTTLE);

	/*
	 * If the caller cannot enter the filesystem, it's possible that it
	 * is due to the caller holding an FS lock or performing a journal
	 * transaction in the case of a filesystem like ext[3|4]. In this case,
	 * it is not safe to block on pfmemalloc_wait as kswapd could be
	 * blocked waiting on the same lock. Instead, throttle for up to a
	 * second before continuing.
	 */
	if (!(gfp_mask & __GFP_FS)) {
		wait_event_interruptible_timeout(pgdat->pfmemalloc_wait,
			pfmemalloc_watermark_ok(pgdat), HZ);

		goto check_pending;
	}

	/* Throttle until kswapd wakes the process */
	wait_event_killable(zone->zone_pgdat->pfmemalloc_wait,
		pfmemalloc_watermark_ok(pgdat));

check_pending:
	if (fatal_signal_pending(current))
		return true;

out:
	return false;
}

unsigned long try_to_free_pages(struct zonelist *zonelist, int order,
				gfp_t gfp_mask, nodemask_t *nodemask)
{
	unsigned long nr_reclaimed;
	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.gfp_mask = (gfp_mask = memalloc_noio_flags(gfp_mask)),
		.order = order,
		.nodemask = nodemask,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
#ifdef CONFIG_DIRECT_RECLAIM_FILE_PAGES_ONLY
		.may_swap = 0,
#else
		.may_swap = 1,
#endif
		.swappiness = vm_swappiness,
	};

	unsigned long pgdatfile = global_page_state(NR_ACTIVE_FILE) +
				global_page_state(NR_INACTIVE_FILE);
	if (pgdatfile <= 12800) {
		sc.may_swap = 1;
	}

	/*
	 * Do not enter reclaim if fatal signal was delivered while throttled.
	 * 1 is returned so that the page allocator does not OOM kill at this
	 * point.
	 */
	if (throttle_direct_reclaim(gfp_mask, zonelist, nodemask))
		return 1;

	trace_mm_vmscan_direct_reclaim_begin(order,
				sc.may_writepage,
				gfp_mask);

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	trace_mm_vmscan_direct_reclaim_end(nr_reclaimed);

	return nr_reclaimed;
}

#ifdef CONFIG_MEMCG

unsigned long mem_cgroup_shrink_node_zone(struct mem_cgroup *memcg,
						gfp_t gfp_mask, bool noswap,
						struct zone *zone,
						unsigned long *nr_scanned)
{
	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.target_mem_cgroup = memcg,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = !noswap,
		.swappiness = vm_swappiness,
	};
	struct lruvec *lruvec = mem_cgroup_zone_lruvec(zone, memcg);
	int swappiness = mem_cgroup_swappiness(memcg);
	unsigned long lru_pages;

	sc.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
			(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK);

	trace_mm_vmscan_memcg_softlimit_reclaim_begin(sc.order,
						      sc.may_writepage,
						      sc.gfp_mask);

	/*
	 * NOTE: Although we can get the priority field, using it
	 * here is not a good idea, since it limits the pages we can scan.
	 * if we don't reclaim here, the shrink_zone from balance_pgdat
	 * will pick up pages from other mem cgroup's as well. We hack
	 * the priority and make it zero.
	 */
	shrink_lruvec(lruvec, swappiness, &sc, &lru_pages);

	trace_mm_vmscan_memcg_softlimit_reclaim_end(sc.nr_reclaimed);

	*nr_scanned = sc.nr_scanned;
	return sc.nr_reclaimed;
}

unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *memcg,
					   unsigned long nr_pages,
					   gfp_t gfp_mask,
					   bool may_swap)
{
	struct zonelist *zonelist;
	unsigned long nr_reclaimed;
	int nid;
	struct scan_control sc = {
		.nr_to_reclaim = max(nr_pages, SWAP_CLUSTER_MAX),
		.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
				(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK),
		.target_mem_cgroup = memcg,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = may_swap,
		.swappiness = vm_swappiness,
	};

	/*
	 * Unlike direct reclaim via alloc_pages(), memcg's reclaim doesn't
	 * take care of from where we get pages. So the node where we start the
	 * scan does not need to be the current node.
	 */
	nid = mem_cgroup_select_victim_node(memcg);

	zonelist = NODE_DATA(nid)->node_zonelists;

	trace_mm_vmscan_memcg_reclaim_begin(0,
					    sc.may_writepage,
					    sc.gfp_mask);

	current->flags |= PF_MEMALLOC;
	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);
	current->flags &= ~PF_MEMALLOC;

	trace_mm_vmscan_memcg_reclaim_end(nr_reclaimed);

	return nr_reclaimed;
}
#endif

static void age_active_anon(struct zone *zone, struct scan_control *sc)
{
	struct mem_cgroup *memcg;

	if (lru_gen_enabled()) {
		lru_gen_age_node(zone->zone_pgdat, sc);
		return;
	}

	if (!total_swap_pages)
		return;

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_zone_lruvec(zone, memcg);

		if (inactive_anon_is_low(lruvec))
			shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
					   sc, LRU_ACTIVE_ANON);

		memcg = mem_cgroup_iter(NULL, memcg, NULL);
	} while (memcg);
}

static bool zone_balanced(struct zone *zone, int order, bool highorder,
			unsigned long balance_gap, int classzone_idx)
{
	unsigned long mark = high_wmark_pages(zone) + balance_gap;

	/*
	 * When checking from pgdat_balanced(), kswapd should stop and sleep
	 * when it reaches the high order-0 watermark and let kcompactd take
	 * over. Other callers such as wakeup_kswapd() want to determine the
	 * true high-order watermark.
	 */
	if (IS_ENABLED(CONFIG_COMPACTION) && !highorder) {
		mark += (1UL << order);
		order = 0;
	}

	return zone_watermark_ok_safe(zone, order, mark, classzone_idx);
}

/*
 * pgdat_balanced() is used when checking if a node is balanced.
 *
 * For order-0, all zones must be balanced!
 *
 * For high-order allocations only zones that meet watermarks and are in a
 * zone allowed by the callers classzone_idx are added to balanced_pages. The
 * total of balanced pages must be at least 25% of the zones allowed by
 * classzone_idx for the node to be considered balanced. Forcing all zones to
 * be balanced for high orders can cause excessive reclaim when there are
 * imbalanced zones.
 * The choice of 25% is due to
 *   o a 16M DMA zone that is balanced will not balance a zone on any
 *     reasonable sized machine
 *   o On all other machines, the top zone must be at least a reasonable
 *     percentage of the middle zones. For example, on 32-bit x86, highmem
 *     would need to be at least 256M for it to be balance a whole node.
 *     Similarly, on x86-64 the Normal zone would need to be at least 1G
 *     to balance a node on its own. These seemed like reasonable ratios.
 */
static bool pgdat_balanced(pg_data_t *pgdat, int order, int classzone_idx)
{
	unsigned long managed_pages = 0;
	unsigned long balanced_pages = 0;
	int i;

	/* Check the watermark levels */
	for (i = 0; i <= classzone_idx; i++) {
		struct zone *zone = pgdat->node_zones + i;

		if (!populated_zone(zone))
			continue;

		managed_pages += zone->managed_pages;

		/*
		 * A special case here:
		 *
		 * balance_pgdat() skips over all_unreclaimable after
		 * DEF_PRIORITY. Effectively, it considers them balanced so
		 * they must be considered balanced here as well!
		 */
		if (!zone_reclaimable(zone)) {
			balanced_pages += zone->managed_pages;
			continue;
		}

		if (zone_balanced(zone, order, false, 0, i))
			balanced_pages += zone->managed_pages;
		else if (!order)
			return false;
	}

	if (order)
#ifdef CONFIG_TIGHT_PGDAT_BALANCE
		return balanced_pages >= (managed_pages >> 1);
#else
		return balanced_pages >= (managed_pages >> 2);
#endif
	else
		return true;
}

/*
 * Prepare kswapd for sleeping. This verifies that there are no processes
 * waiting in throttle_direct_reclaim() and that watermarks have been met.
 *
 * Returns true if kswapd is ready to sleep
 */
static bool prepare_kswapd_sleep(pg_data_t *pgdat, int order, long remaining,
					int classzone_idx)
{
	/* If a direct reclaimer woke kswapd within HZ/10, it's premature */
	if (remaining)
		return false;

	/*
	 * The throttled processes are normally woken up in balance_pgdat() as
	 * soon as pfmemalloc_watermark_ok() is true. But there is a potential
	 * race between when kswapd checks the watermarks and a process gets
	 * throttled. There is also a potential race if processes get
	 * throttled, kswapd wakes, a large process exits thereby balancing the
	 * zones, which causes kswapd to exit balance_pgdat() before reaching
	 * the wake up checks. If kswapd is going to sleep, no process should
	 * be sleeping on pfmemalloc_wait, so wake them now if necessary. If
	 * the wake up is premature, processes will wake kswapd and get
	 * throttled again. The difference from wake ups in balance_pgdat() is
	 * that here we are under prepare_to_wait().
	 */
	if (waitqueue_active(&pgdat->pfmemalloc_wait))
		wake_up_all(&pgdat->pfmemalloc_wait);

	return pgdat_balanced(pgdat, order, classzone_idx);
}

/*
 * kswapd shrinks the zone by the number of pages required to reach
 * the high watermark.
 *
 * Returns true if kswapd scanned at least the requested number of pages to
 * reclaim or if the lack of progress was due to pages under writeback.
 * This is used to determine if the scanning priority needs to be raised.
 */
static bool kswapd_shrink_zone(struct zone *zone,
			       int classzone_idx,
			       struct scan_control *sc)
{
	unsigned long balance_gap;
	bool lowmem_pressure;

	/* Reclaim above the high watermark. */
	sc->nr_to_reclaim = max(SWAP_CLUSTER_MAX, high_wmark_pages(zone));

	/*
	 * We put equal pressure on every zone, unless one zone has way too
	 * many pages free already. The "too many pages" is defined as the
	 * high wmark plus a "gap" where the gap is either the low
	 * watermark or 1% of the zone, whichever is smaller.
	 */
	balance_gap = min(low_wmark_pages(zone), DIV_ROUND_UP(
			zone->managed_pages, KSWAPD_ZONE_BALANCE_GAP_RATIO));

	/*
	 * If there is no low memory pressure or the zone is balanced then no
	 * reclaim is necessary
	 */
	lowmem_pressure = (buffer_heads_over_limit && is_highmem(zone));
	if (!lowmem_pressure && zone_balanced(zone, sc->order, false,
						balance_gap, classzone_idx))
		return true;

	shrink_zone(zone, sc, zone_idx(zone) == classzone_idx);

	clear_bit(ZONE_WRITEBACK, &zone->flags);

	/*
	 * If a zone reaches its high watermark, consider it to be no longer
	 * congested. It's possible there are dirty pages backed by congested
	 * BDIs but as pressure is relieved, speculatively avoid congestion
	 * waits.
	 */
	if (zone_reclaimable(zone) &&
	    zone_balanced(zone, sc->order, false, 0, classzone_idx)) {
		clear_bit(ZONE_CONGESTED, &zone->flags);
		clear_bit(ZONE_DIRTY, &zone->flags);
	}

	return sc->nr_scanned >= sc->nr_to_reclaim;
}

/*
 * For kswapd, balance_pgdat() will work across all this node's zones until
 * they are all at high_wmark_pages(zone).
 *
 * Returns the highest zone idx kswapd was reclaiming at
 *
 * There is special handling here for zones which are full of pinned pages.
 * This can happen if the pages are all mlocked, or if they are all used by
 * device drivers (say, ZONE_DMA).  Or if they are all in use by hugetlb.
 * What we do is to detect the case where all pages in the zone have been
 * scanned twice and there has been zero successful reclaim.  Mark the zone as
 * dead and from now on, only perform a short scan.  Basically we're polling
 * the zone for when the problem goes away.
 *
 * kswapd scans the zones in the highmem->normal->dma direction.  It skips
 * zones which have free_pages > high_wmark_pages(zone), but once a zone is
 * found to have free_pages <= high_wmark_pages(zone), we scan that zone and the
 * lower zones regardless of the number of free pages in the lower zones. This
 * interoperates with the page allocator fallback scheme to ensure that aging
 * of pages is balanced across the zones.
 */
static int balance_pgdat(pg_data_t *pgdat, int order, int classzone_idx)
{
	int i;
	int end_zone = 0;	/* Inclusive.  0 = ZONE_DMA */
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	unsigned long pflags;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.order = order,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = 1,
		.swappiness = vm_swappiness,
	};

	/*
	 * Account kswapd reclaim as memory stall time (upstream v5.4
	 * placement). Without this, periods where only kswapd is working
	 * contribute nothing to /proc/pressure/memory, under-reporting
	 * "some" pressure exactly when a small-RAM device needs lmkd's
	 * PSI monitor to see it.
	 */
	psi_memstall_enter(&pflags);

	count_vm_event(PAGEOUTRUN);

	do {
		bool raise_priority = true;

		sc.nr_reclaimed = 0;

		/*
		 * Scan in the highmem->dma direction for the highest
		 * zone which needs scanning
		 */
		for (i = pgdat->nr_zones - 1; i >= 0; i--) {
			struct zone *zone = pgdat->node_zones + i;

			if (!populated_zone(zone))
				continue;

			if (sc.priority != DEF_PRIORITY &&
			    !zone_reclaimable(zone))
				continue;

			/*
			 * Do some background aging of the anon list, to give
			 * pages a chance to be referenced before reclaiming.
			 */
			age_active_anon(zone, &sc);

			/*
			 * If the number of buffer_heads in the machine
			 * exceeds the maximum allowed level and this node
			 * has a highmem zone, force kswapd to reclaim from
			 * it to relieve lowmem pressure.
			 */
			if (buffer_heads_over_limit && is_highmem_idx(i)) {
				end_zone = i;
				break;
			}

			if (!zone_balanced(zone, order, false, 0, 0)) {
				end_zone = i;
				break;
			} else {
				/*
				 * If balanced, clear the dirty and congested
				 * flags
				 */
				clear_bit(ZONE_CONGESTED, &zone->flags);
				clear_bit(ZONE_DIRTY, &zone->flags);
			}
		}

		if (i < 0)
			goto out;

		/*
		 * If we're getting trouble reclaiming, start doing writepage
		 * even in laptop mode.
		 */
		if (sc.priority < DEF_PRIORITY - 2)
			sc.may_writepage = 1;

		/*
		 * Now scan the zone in the dma->highmem direction, stopping
		 * at the last zone which needs scanning.
		 *
		 * We do this because the page allocator works in the opposite
		 * direction.  This prevents the page allocator from allocating
		 * pages behind kswapd's direction of progress, which would
		 * cause too much scanning of the lower zones.
		 */
		for (i = 0; i <= end_zone; i++) {
			struct zone *zone = pgdat->node_zones + i;

			if (!populated_zone(zone))
				continue;

			if (sc.priority != DEF_PRIORITY &&
			    !zone_reclaimable(zone))
				continue;

			sc.nr_scanned = 0;

			nr_soft_scanned = 0;
			/*
			 * Call soft limit reclaim before calling shrink_zone.
			 */
			nr_soft_reclaimed = mem_cgroup_soft_limit_reclaim(zone,
							order, sc.gfp_mask,
							&nr_soft_scanned);
			sc.nr_reclaimed += nr_soft_reclaimed;

			/*
			 * There should be no need to raise the scanning
			 * priority if enough pages are already being scanned
			 * that that high watermark would be met at 100%
			 * efficiency.
			 */
			if (kswapd_shrink_zone(zone, end_zone, &sc))
				raise_priority = false;
		}

		/*
		 * If the low watermark is met there is no need for processes
		 * to be throttled on pfmemalloc_wait as they should not be
		 * able to safely make forward progress. Wake them
		 */
		if (waitqueue_active(&pgdat->pfmemalloc_wait) &&
				pfmemalloc_watermark_ok(pgdat))
			wake_up_all(&pgdat->pfmemalloc_wait);

		/* Check if kswapd should be suspending */
		if (try_to_freeze() || kthread_should_stop() ||
		    !atomic_read(&pgdat->kswapd_waiters))
			break;

		/*
		 * Raise priority if scanning rate is too low or there was no
		 * progress in reclaiming pages
		 */
		if (raise_priority || !sc.nr_reclaimed)
			sc.priority--;
	} while (sc.priority >= 1 &&
			!pgdat_balanced(pgdat, order, classzone_idx));

out:
	psi_memstall_leave(&pflags);

	/*
	 * Return the highest zone idx we were reclaiming at so
	 * prepare_kswapd_sleep() makes the same decisions as here.
	 */
	return end_zone;
}

static void kswapd_try_to_sleep(pg_data_t *pgdat, int order,
				int classzone_idx, int balanced_classzone_idx)
{
	long remaining = 0;
	DEFINE_WAIT(wait);

	if (freezing(current) || kthread_should_stop())
		return;

	prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);

	/* Try to sleep for a short interval */
	if (prepare_kswapd_sleep(pgdat, order, remaining,
						balanced_classzone_idx)) {
		/*
		 * Compaction records what page blocks it recently failed to
		 * isolate pages from and skips them in the future scanning.
		 * When kswapd is going to sleep, it is reasonable to assume
		 * that pages and compaction may succeed so reset the cache.
		 */
		reset_isolation_suitable(pgdat);

		/*
		 * We have freed the memory, now we should compact it to make
		 * allocation of the requested order possible.
		 */
		wakeup_kcompactd(pgdat, order, classzone_idx);

		remaining = schedule_timeout(HZ/10);
		finish_wait(&pgdat->kswapd_wait, &wait);
		prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);
	}

	/*
	 * After a short sleep, check if it was a premature sleep. If not, then
	 * go fully to sleep until explicitly woken up.
	 */
	if (prepare_kswapd_sleep(pgdat, order, remaining,
						balanced_classzone_idx)) {
		trace_mm_vmscan_kswapd_sleep(pgdat->node_id);

		/*
		 * vmstat counters are not perfectly accurate and the estimated
		 * value for counters such as NR_FREE_PAGES can deviate from the
		 * true value by nr_online_cpus * threshold. To avoid the zone
		 * watermarks being breached while under pressure, we reduce the
		 * per-cpu vmstat threshold while kswapd is awake and restore
		 * them before going back to sleep.
		 */
		set_pgdat_percpu_threshold(pgdat, calculate_normal_threshold);

		if (!kthread_should_stop())
			schedule();

		set_pgdat_percpu_threshold(pgdat, calculate_pressure_threshold);
	} else {
		if (remaining)
			count_vm_event(KSWAPD_LOW_WMARK_HIT_QUICKLY);
		else
			count_vm_event(KSWAPD_HIGH_WMARK_HIT_QUICKLY);
	}
	finish_wait(&pgdat->kswapd_wait, &wait);
}

#if CONFIG_KSWAPD_CPU
static struct cpumask kswapd_cpumask;
static void init_kswapd_cpumask(void)
{
	int i;

	cpumask_clear(&kswapd_cpumask);
	for (i = 0; i < nr_cpu_ids; i++) {
		if (CONFIG_KSWAPD_CPU & (1 << i))
			cpumask_set_cpu(i, &kswapd_cpumask);
	}
}
#endif

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process.
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
static int kswapd(void *p)
{
	unsigned long order, new_order;
	int classzone_idx, new_classzone_idx;
	int balanced_classzone_idx;
	pg_data_t *pgdat = (pg_data_t*)p;
	struct task_struct *tsk = current;

	struct reclaim_state reclaim_state = {
		.reclaimed_slab = 0,
	};
#if CONFIG_KSWAPD_CPU
	const struct cpumask *cpumask = &kswapd_cpumask;
#else
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);
#endif

	lockdep_set_current_reclaim_state(GFP_KERNEL);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);
	current->reclaim_state = &reclaim_state;

	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 *
	 * (Kswapd normally doesn't need memory anyway, but sometimes
	 * you need a small amount of memory in order to be able to
	 * page out something else, and this flag essentially protects
	 * us from recursively trying to free more memory as we're
	 * trying to free the first piece of memory in the first place).
	 */
	tsk->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	set_freezable();

	order = new_order = 0;
	classzone_idx = new_classzone_idx = pgdat->nr_zones - 1;
	balanced_classzone_idx = classzone_idx;
	for ( ; ; ) {
		bool ret;

		/*
		 * While we were reclaiming, there might have been another
		 * wakeup, so check the values.
		 */
		new_order = pgdat->kswapd_max_order;
		new_classzone_idx = pgdat->classzone_idx;
		pgdat->kswapd_max_order =  0;
		pgdat->classzone_idx = pgdat->nr_zones - 1;

		if (order < new_order || classzone_idx > new_classzone_idx) {
			/*
			 * Don't sleep if someone wants a larger 'order'
			 * allocation or has tigher zone constraints
			 */
			order = new_order;
			classzone_idx = new_classzone_idx;
		} else {
			kswapd_try_to_sleep(pgdat, order, classzone_idx,
						balanced_classzone_idx);
			order = pgdat->kswapd_max_order;
			classzone_idx = pgdat->classzone_idx;
			new_order = order;
			new_classzone_idx = classzone_idx;
			pgdat->kswapd_max_order = 0;
			pgdat->classzone_idx = pgdat->nr_zones - 1;
		}

		ret = try_to_freeze();
		if (kthread_should_stop())
			break;

		/*
		 * We can speed up thawing tasks if we don't call balance_pgdat
		 * after returning from the refrigerator
		 */
		if (!ret) {
			trace_mm_vmscan_kswapd_wake(pgdat->node_id, order);
			balanced_classzone_idx = balance_pgdat(pgdat, order,
								classzone_idx);
		}
	}

	tsk->flags &= ~(PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD);
	current->reclaim_state = NULL;
	lockdep_clear_current_reclaim_state();

	return 0;
}

/*
 * A zone is low on free memory, so wake its kswapd task to service it.
 */
void wakeup_kswapd(struct zone *zone, int order, enum zone_type classzone_idx)
{
	pg_data_t *pgdat;

	if (!populated_zone(zone))
		return;

	if (!cpuset_zone_allowed(zone, GFP_KERNEL | __GFP_HARDWALL))
		return;
	pgdat = zone->zone_pgdat;
	if (pgdat->kswapd_max_order < order) {
		pgdat->kswapd_max_order = order;
		pgdat->classzone_idx = min(pgdat->classzone_idx, classzone_idx);
	}
	if (!waitqueue_active(&pgdat->kswapd_wait))
		return;
	if (zone_balanced(zone, order, true, 0, 0))
		return;

	trace_mm_vmscan_wakeup_kswapd(pgdat->node_id, zone_idx(zone), order);
	wake_up_interruptible(&pgdat->kswapd_wait);
}

#ifdef CONFIG_HIBERNATION
/*
 * Try to free `nr_to_reclaim' of memory, system-wide, and return the number of
 * freed pages.
 *
 * Rather than trying to age LRUs the aim is to preserve the overall
 * LRU order by reclaiming preferentially
 * inactive > active > active referenced > active mapped
 */
unsigned long shrink_all_memory(unsigned long nr_to_reclaim)
{
	struct reclaim_state reclaim_state = {
		.reclaimed_slab = 0,
	};
	struct scan_control sc = {
		.nr_to_reclaim = nr_to_reclaim,
		.gfp_mask = GFP_HIGHUSER_MOVABLE,
		.priority = DEF_PRIORITY,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.swappiness = vm_swappiness,
		.hibernation_mode = 1,
	};
	struct zonelist *zonelist = node_zonelist(numa_node_id(), sc.gfp_mask);
	struct task_struct *p = current;
	unsigned long nr_reclaimed;

	p->flags |= PF_MEMALLOC;
	lockdep_set_current_reclaim_state(sc.gfp_mask);
	reclaim_state.reclaimed_slab = 0;
	p->reclaim_state = &reclaim_state;

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	p->reclaim_state = NULL;
	lockdep_clear_current_reclaim_state();
	p->flags &= ~PF_MEMALLOC;

	return nr_reclaimed;
}
#endif /* CONFIG_HIBERNATION */

/* It's optimal to keep kswapds on the same CPUs as their memory, but
   not required for correctness.  So if the last cpu in a node goes
   away, we get changed to run anywhere: as the first one comes back,
   restore their cpu bindings. */
static int cpu_callback(struct notifier_block *nfb, unsigned long action,
			void *hcpu)
{
	int nid;

	if (action == CPU_ONLINE || action == CPU_ONLINE_FROZEN) {
		for_each_node_state(nid, N_MEMORY) {
			pg_data_t *pgdat = NODE_DATA(nid);
			const struct cpumask *mask;

#if CONFIG_KSWAPD_CPU
			mask = &kswapd_cpumask;
#else
			mask = cpumask_of_node(pgdat->node_id);
#endif

			if (cpumask_any_and(cpu_online_mask, mask) < nr_cpu_ids)
				/* One of our CPUs online: restore mask */
				set_cpus_allowed_ptr(pgdat->kswapd, mask);
		}
	}
	return NOTIFY_OK;
}

/*
 * This kswapd start function will be called by init and node-hot-add.
 * On node-hot-add, kswapd will moved to proper cpus if cpus are hot-added.
 */
int kswapd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int ret = 0;

	if (pgdat->kswapd)
		return 0;

	pgdat->kswapd = kthread_run(kswapd, pgdat, "kswapd%d", nid);
	if (IS_ERR(pgdat->kswapd)) {
		/* failure at boot is fatal */
		BUG_ON(system_state == SYSTEM_BOOTING);
		pr_err("Failed to start kswapd on node %d\n", nid);
		ret = PTR_ERR(pgdat->kswapd);
		pgdat->kswapd = NULL;
	}
	return ret;
}

/*
 * Called by memory hotplug when all memory in a node is offlined.  Caller must
 * hold mem_hotplug_begin/end().
 */
void kswapd_stop(int nid)
{
	struct task_struct *kswapd = NODE_DATA(nid)->kswapd;

	if (kswapd) {
		kthread_stop(kswapd);
		NODE_DATA(nid)->kswapd = NULL;
	}
}

static int __init kswapd_init(void)
{
	int nid;

#if CONFIG_KSWAPD_CPU
	init_kswapd_cpumask();
#endif
	swap_setup();
	for_each_node_state(nid, N_MEMORY)
 		kswapd_run(nid);
	hotcpu_notifier(cpu_callback, 0);
#ifdef CONFIG_SYSFS
	if (sysfs_create_group(mm_kobj, &vmscan_attr_group))
		pr_err("vmscan: register sysfs failed\n");
#endif
	return 0;
}

module_init(kswapd_init)

#ifdef CONFIG_NUMA
/*
 * Zone reclaim mode
 *
 * If non-zero call zone_reclaim when the number of free pages falls below
 * the watermarks.
 */
int zone_reclaim_mode __read_mostly;

#define RECLAIM_OFF 0
#define RECLAIM_ZONE (1<<0)	/* Run shrink_inactive_list on the zone */
#define RECLAIM_WRITE (1<<1)	/* Writeout pages during reclaim */
#define RECLAIM_UNMAP (1<<2)	/* Unmap pages during reclaim */

/*
 * Priority for ZONE_RECLAIM. This determines the fraction of pages
 * of a node considered for each zone_reclaim. 4 scans 1/16th of
 * a zone.
 */
#define ZONE_RECLAIM_PRIORITY 4

/*
 * Percentage of pages in a zone that must be unmapped for zone_reclaim to
 * occur.
 */
int sysctl_min_unmapped_ratio = 1;

/*
 * If the number of slab pages in a zone grows beyond this percentage then
 * slab reclaim needs to occur.
 */
int sysctl_min_slab_ratio = 5;

static inline unsigned long zone_unmapped_file_pages(struct zone *zone)
{
	unsigned long file_mapped = zone_page_state(zone, NR_FILE_MAPPED);
	unsigned long file_lru = zone_page_state(zone, NR_INACTIVE_FILE) +
		zone_page_state(zone, NR_ACTIVE_FILE);

	/*
	 * It's possible for there to be more file mapped pages than
	 * accounted for by the pages on the file LRU lists because
	 * tmpfs pages accounted for as ANON can also be FILE_MAPPED
	 */
	return (file_lru > file_mapped) ? (file_lru - file_mapped) : 0;
}

/* Work out how many page cache pages we can reclaim in this reclaim_mode */
static unsigned long zone_pagecache_reclaimable(struct zone *zone)
{
	unsigned long nr_pagecache_reclaimable;
	unsigned long delta = 0;

	/*
	 * If RECLAIM_UNMAP is set, then all file pages are considered
	 * potentially reclaimable. Otherwise, we have to worry about
	 * pages like swapcache and zone_unmapped_file_pages() provides
	 * a better estimate
	 */
	if (zone_reclaim_mode & RECLAIM_UNMAP)
		nr_pagecache_reclaimable = zone_page_state(zone, NR_FILE_PAGES);
	else
		nr_pagecache_reclaimable = zone_unmapped_file_pages(zone);

	/* If we can't clean pages, remove dirty pages from consideration */
	if (!(zone_reclaim_mode & RECLAIM_WRITE))
		delta += zone_page_state(zone, NR_FILE_DIRTY);

	/* Watch for any possible underflows due to delta */
	if (unlikely(delta > nr_pagecache_reclaimable))
		delta = nr_pagecache_reclaimable;

	return nr_pagecache_reclaimable - delta;
}

/*
 * Try to free up some pages from this zone through reclaim.
 */
static int __zone_reclaim(struct zone *zone, gfp_t gfp_mask, unsigned int order)
{
	/* Minimum pages needed in order to stay on node */
	const unsigned long nr_pages = 1 << order;
	struct task_struct *p = current;
	struct reclaim_state reclaim_state = {
		.reclaimed_slab = 0,
	};
	struct scan_control sc = {
		.nr_to_reclaim = max(nr_pages, SWAP_CLUSTER_MAX),
		.gfp_mask = (gfp_mask = memalloc_noio_flags(gfp_mask)),
		.order = order,
		.swappiness = vm_swappiness,
		.priority = ZONE_RECLAIM_PRIORITY,
		.may_writepage = !!(zone_reclaim_mode & RECLAIM_WRITE),
		.may_unmap = !!(zone_reclaim_mode & RECLAIM_UNMAP),
		.may_swap = 1,
	};

	cond_resched();
	/*
	 * We need to be able to allocate from the reserves for RECLAIM_UNMAP
	 * and we also need to be able to write out pages for RECLAIM_WRITE
	 * and RECLAIM_UNMAP.
	 */
	p->flags |= PF_MEMALLOC | PF_SWAPWRITE;
	lockdep_set_current_reclaim_state(gfp_mask);
	reclaim_state.reclaimed_slab = 0;
	p->reclaim_state = &reclaim_state;

	if (zone_pagecache_reclaimable(zone) > zone->min_unmapped_pages) {
		/*
		 * Free memory by calling shrink zone with increasing
		 * priorities until we have enough memory freed.
		 */
		do {
			shrink_zone(zone, &sc, true);
		} while (sc.nr_reclaimed < nr_pages && --sc.priority >= 0);
	}

	p->reclaim_state = NULL;
	current->flags &= ~(PF_MEMALLOC | PF_SWAPWRITE);
	lockdep_clear_current_reclaim_state();
	return sc.nr_reclaimed >= nr_pages;
}

int zone_reclaim(struct zone *zone, gfp_t gfp_mask, unsigned int order)
{
	int node_id;
	int ret;

	/*
	 * Zone reclaim reclaims unmapped file backed pages and
	 * slab pages if we are over the defined limits.
	 *
	 * A small portion of unmapped file backed pages is needed for
	 * file I/O otherwise pages read by file I/O will be immediately
	 * thrown out if the zone is overallocated. So we do not reclaim
	 * if less than a specified percentage of the zone is used by
	 * unmapped file backed pages.
	 */
	if (zone_pagecache_reclaimable(zone) <= zone->min_unmapped_pages &&
	    zone_page_state(zone, NR_SLAB_RECLAIMABLE) <= zone->min_slab_pages)
		return ZONE_RECLAIM_FULL;

	if (!zone_reclaimable(zone))
		return ZONE_RECLAIM_FULL;

	/*
	 * Do not scan if the allocation should not be delayed.
	 */
	if (!gfpflags_allow_blocking(gfp_mask) || (current->flags & PF_MEMALLOC))
		return ZONE_RECLAIM_NOSCAN;

	/*
	 * Only run zone reclaim on the local zone or on zones that do not
	 * have associated processors. This will favor the local processor
	 * over remote processors and spread off node memory allocations
	 * as wide as possible.
	 */
	node_id = zone_to_nid(zone);
	if (node_state(node_id, N_CPU) && node_id != numa_node_id())
		return ZONE_RECLAIM_NOSCAN;

	if (test_and_set_bit(ZONE_RECLAIM_LOCKED, &zone->flags))
		return ZONE_RECLAIM_NOSCAN;

	ret = __zone_reclaim(zone, gfp_mask, order);
	clear_bit_unlock(ZONE_RECLAIM_LOCKED, &zone->flags);

	if (!ret)
		count_vm_event(PGSCAN_ZONE_RECLAIM_FAILED);

	return ret;
}
#endif

/*
 * page_evictable - test whether a page is evictable
 * @page: the page to test
 *
 * Test whether page is evictable--i.e., should be placed on active/inactive
 * lists vs unevictable list.
 *
 * Reasons page might not be evictable:
 * (1) page's mapping marked unevictable
 * (2) page is part of an mlocked VMA
 *
 */
int page_evictable(struct page *page)
{
	int ret;

	/* Prevent address_space of inode and swap cache from being freed */
	rcu_read_lock();
	ret = !mapping_unevictable(page_mapping(page)) && !PageMlocked(page);
	rcu_read_unlock();
	return ret;
}

#ifdef CONFIG_SHMEM
/**
 * check_move_unevictable_pages - check pages for evictability and move to appropriate zone lru list
 * @pages:	array of pages to check
 * @nr_pages:	number of pages to check
 *
 * Checks pages for evictability and moves them to the appropriate lru list.
 *
 * This function is only used for SysV IPC SHM_UNLOCK.
 */
void check_move_unevictable_pages(struct page **pages, int nr_pages)
{
	struct lruvec *lruvec;
	struct zone *zone = NULL;
	int pgscanned = 0;
	int pgrescued = 0;
	int i;

	for (i = 0; i < nr_pages; i++) {
		struct page *page = pages[i];
		struct zone *pagezone;

		pgscanned++;
		pagezone = page_zone(page);
		if (pagezone != zone) {
			if (zone)
				spin_unlock_irq(&zone->lru_lock);
			zone = pagezone;
			spin_lock_irq(&zone->lru_lock);
		}
		lruvec = mem_cgroup_page_lruvec(page, zone);

		if (!PageLRU(page) || !PageUnevictable(page))
			continue;

		if (page_evictable(page)) {
			enum lru_list lru = page_lru_base_type(page);

			VM_BUG_ON_PAGE(PageActive(page), page);
			ClearPageUnevictable(page);
			del_page_from_lru_list(page, lruvec, LRU_UNEVICTABLE);
			add_page_to_lru_list(page, lruvec, lru);
			pgrescued++;
		}
	}

	if (zone) {
		__count_vm_events(UNEVICTABLE_PGRESCUED, pgrescued);
		__count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
		spin_unlock_irq(&zone->lru_lock);
	}
}
#endif /* CONFIG_SHMEM */
