// SPDX-License-Identifier: GPL-2.0
/*
 * 4.4 backport note: backported from v5.15.189 mm/page_vma_mapped.c (plus
 * vma_address_end() from v5.15.189 mm/internal.h) as the page_vma_mapped_walk()
 * prerequisite of the MGLRU port.
 *
 * Adaptations for this tree: huge_pte_offset() takes the 4.4 two-argument
 * form and compound-page sizes use hpage_nr_pages() (there is no page_size()
 * or thp_nr_pages() here); the ZONE_DEVICE (device-private/device-exclusive)
 * and PMD-migration/THP-migration branches have no 4.4 counterpart and are
 * dropped; the p4d level is folded into the pgd (definitions in
 * include/linux/pagewalk.h); page_mapped_in_vma() stays in mm/rmap.c.
 */
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/pagemap.h>
#include <linux/pagewalk.h>

static inline bool not_found(struct page_vma_mapped_walk *pvmw)
{
	page_vma_mapped_walk_done(pvmw);
	return false;
}

static bool map_pte(struct page_vma_mapped_walk *pvmw)
{
	pvmw->pte = pte_offset_map(pvmw->pmd, pvmw->address);
	if (!(pvmw->flags & PVMW_SYNC)) {
		if (pvmw->flags & PVMW_MIGRATION) {
			if (!is_swap_pte(*pvmw->pte))
				return false;
		} else if (!pte_present(*pvmw->pte))
			return false;
	}
	pvmw->ptl = pte_lockptr(pvmw->vma->vm_mm, pvmw->pmd);
	spin_lock(pvmw->ptl);
	return true;
}

static inline bool pfn_is_match(struct page *page, unsigned long pfn)
{
	unsigned long page_pfn = page_to_pfn(page);

	/* normal page and hugetlbfs page */
	if (!PageTransCompound(page) || PageHuge(page))
		return page_pfn == pfn;

	/* THP can be referenced by any subpage */
	return pfn >= page_pfn && pfn - page_pfn < hpage_nr_pages(page);
}

/**
 * check_pte - check if @pvmw->page is mapped at the @pvmw->pte
 * @pvmw: page_vma_mapped_walk struct, includes a pair pte and page for checking
 *
 * page_vma_mapped_walk() found a place where @pvmw->page is *potentially*
 * mapped. check_pte() has to validate this.
 *
 * pvmw->pte may point to empty PTE, swap PTE or PTE pointing to
 * arbitrary page.
 *
 * If PVMW_MIGRATION flag is set, returns true if @pvmw->pte contains migration
 * entry that points to @pvmw->page or any subpage in case of THP.
 *
 * If PVMW_MIGRATION flag is not set, returns true if pvmw->pte points to
 * pvmw->page or any subpage in case of THP.
 *
 * Otherwise, return false.
 *
 */
static bool check_pte(struct page_vma_mapped_walk *pvmw)
{
	unsigned long pfn;

	if (pvmw->flags & PVMW_MIGRATION) {
		swp_entry_t entry;
		if (!is_swap_pte(*pvmw->pte))
			return false;
		entry = pte_to_swp_entry(*pvmw->pte);

		if (!is_migration_entry(entry))
			return false;

		pfn = swp_offset(entry);
	} else {
		if (!pte_present(*pvmw->pte))
			return false;

		pfn = pte_pfn(*pvmw->pte);
	}

	return pfn_is_match(pvmw->page, pfn);
}

static void step_forward(struct page_vma_mapped_walk *pvmw, unsigned long size)
{
	pvmw->address = (pvmw->address + size) & ~(size - 1);
	if (!pvmw->address)
		pvmw->address = ULONG_MAX;
}

/*
 * Then at what user virtual address will none of the page be found in vma?
 * Assumes that vma_address() already returned a good starting address.
 * If page is a compound head, the entire compound page is considered.
 */
static inline unsigned long
vma_address_end(struct page *page, struct vm_area_struct *vma)
{
	pgoff_t pgoff;
	unsigned long address;

	VM_BUG_ON_PAGE(PageKsm(page), page);	/* KSM page->index unusable */
	pgoff = page_to_pgoff(page) + hpage_nr_pages(page);
	address = vma->vm_start + ((pgoff - vma->vm_pgoff) << PAGE_SHIFT);
	/* Check for address beyond vma (or wrapped through 0?) */
	if (address < vma->vm_start || address > vma->vm_end)
		address = vma->vm_end;
	return address;
}

/**
 * page_vma_mapped_walk - check if @pvmw->page is mapped in @pvmw->vma at
 * @pvmw->address
 * @pvmw: pointer to struct page_vma_mapped_walk. page, vma, address and flags
 * must be set. pmd, pte and ptl must be NULL.
 *
 * Returns true if the page is mapped in the vma. @pvmw->pmd and @pvmw->pte point
 * to relevant page table entries. @pvmw->ptl is locked. @pvmw->address is
 * adjusted if needed (for PTE-mapped THPs).
 *
 * If @pvmw->pmd is set but @pvmw->pte is not, you have found PMD-mapped page
 * (usually THP). For PTE-mapped THP, you should run page_vma_mapped_walk() in
 * a loop to find all PTEs that map the THP.
 *
 * For HugeTLB pages, @pvmw->pte is set to the relevant page table entry
 * regardless of which page table level the page is mapped at. @pvmw->pmd is
 * NULL.
 *
 * Returns false if there are no more page table entries for the page in
 * the vma. @pvmw->ptl is unlocked and @pvmw->pte is unmapped.
 *
 * If you need to stop the walk before page_vma_mapped_walk() returned false,
 * use page_vma_mapped_walk_done(). It will do the housekeeping.
 */
bool page_vma_mapped_walk(struct page_vma_mapped_walk *pvmw)
{
	struct mm_struct *mm = pvmw->vma->vm_mm;
	struct page *page = pvmw->page;
	unsigned long end;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t pmde;

	/* The only possible pmd mapping has been handled on last iteration */
	if (pvmw->pmd && !pvmw->pte)
		return not_found(pvmw);

	if (unlikely(PageHuge(page))) {
		/* The only possible mapping was handled on last iteration */
		if (pvmw->pte)
			return not_found(pvmw);

		/* when pud is not present, pte will be NULL */
		pvmw->pte = huge_pte_offset(mm, pvmw->address);
		if (!pvmw->pte)
			return false;

		pvmw->ptl = huge_pte_lockptr(page_hstate(page), mm, pvmw->pte);
		spin_lock(pvmw->ptl);
		if (!check_pte(pvmw))
			return not_found(pvmw);
		return true;
	}

	/*
	 * Seek to next pte only makes sense for THP.
	 * But more important than that optimization, is to filter out
	 * any PageKsm page: whose page->index misleads vma_address()
	 * and vma_address_end() to disaster.
	 */
	end = PageTransCompound(page) ?
		vma_address_end(page, pvmw->vma) :
		pvmw->address + PAGE_SIZE;
	if (pvmw->pte)
		goto next_pte;
restart:
	do {
		pgd = pgd_offset(mm, pvmw->address);
		if (!pgd_present(*pgd)) {
			step_forward(pvmw, PGDIR_SIZE);
			continue;
		}
		p4d = p4d_offset(pgd, pvmw->address);
		if (!p4d_present(*p4d)) {
			step_forward(pvmw, P4D_SIZE);
			continue;
		}
		pud = pud_offset(p4d, pvmw->address);
		if (!pud_present(*pud)) {
			step_forward(pvmw, PUD_SIZE);
			continue;
		}

		pvmw->pmd = pmd_offset(pud, pvmw->address);
		/*
		 * Make sure the pmd value isn't cached in a register by the
		 * compiler and used as a stale value after we've observed a
		 * subsequent update.
		 */
		pmde = READ_ONCE(*pvmw->pmd);

		if (pmd_trans_huge(pmde)) {
			pvmw->ptl = pmd_lock(mm, pvmw->pmd);
			pmde = *pvmw->pmd;
			if (likely(pmd_trans_huge(pmde))) {
				if (pvmw->flags & PVMW_MIGRATION)
					return not_found(pvmw);
				if (pmd_page(pmde) != page)
					return not_found(pvmw);
				return true;
			}
			if (!pmd_present(pmde))
				return not_found(pvmw);
			/* THP pmd was split under us: handle on pte level */
			spin_unlock(pvmw->ptl);
			pvmw->ptl = NULL;
		} else if (!pmd_present(pmde)) {
			/*
			 * If PVMW_SYNC, take and drop THP pmd lock so that we
			 * cannot return prematurely, while zap_huge_pmd() has
			 * cleared *pmd but not decremented compound_mapcount().
			 */
			if ((pvmw->flags & PVMW_SYNC) &&
			    PageTransCompound(page)) {
				spinlock_t *ptl = pmd_lock(mm, pvmw->pmd);

				spin_unlock(ptl);
			}
			step_forward(pvmw, PMD_SIZE);
			continue;
		}
		if (!map_pte(pvmw))
			goto next_pte;
this_pte:
		if (check_pte(pvmw))
			return true;
next_pte:
		do {
			pvmw->address += PAGE_SIZE;
			if (pvmw->address >= end)
				return not_found(pvmw);
			/* Did we cross page table boundary? */
			if ((pvmw->address & (PMD_SIZE - PAGE_SIZE)) == 0) {
				if (pvmw->ptl) {
					spin_unlock(pvmw->ptl);
					pvmw->ptl = NULL;
				}
				pte_unmap(pvmw->pte);
				pvmw->pte = NULL;
				goto restart;
			}
			pvmw->pte++;
			if ((pvmw->flags & PVMW_SYNC) && !pvmw->ptl) {
				pvmw->ptl = pte_lockptr(mm, pvmw->pmd);
				spin_lock(pvmw->ptl);
			}
		} while (pte_none(*pvmw->pte));

		if (!pvmw->ptl) {
			pvmw->ptl = pte_lockptr(mm, pvmw->pmd);
			spin_lock(pvmw->ptl);
		}
		goto this_pte;
	} while (pvmw->address < end);

	return false;
}
