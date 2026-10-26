/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MM_P3S_H
#define __MM_P3S_H

#include <linux/p3s/vma.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/highmem.h>
#include <linux/userfaultfd_k.h>
#include <linux/huge_mm.h>
#include <linux/pgtable.h>

/*
 * vma_pte_add_slice - Apply subpage slice offset to a PTE value
 * @vma: Pointer to struct vm_area_struct
 * @addr: Virtual address within @vma
 * @pte: Base page table entry
 *
 * In 4KB compatibility mode, encodes the subpage slice index (0..3) into
 * the PTE physical frame bits.
 */
static inline pte_t vma_pte_add_slice(const struct vm_area_struct *vma,
				      unsigned long addr,
				      pte_t pte)
{
	if (vma_is_p3s_4k(vma)) {
		unsigned int slice = vma_slice_offset(vma, addr);

		return __pte(pte_val(pte) + ((pteval_t)slice << PAGE_SHIFT_4KB));
	}
	return pte;
}

/*
 * vma_folio_mk_pte - Construct PTE for a folio mapped at virtual address
 * @vma: Pointer to struct vm_area_struct
 * @folio: Host physical folio
 * @addr: Virtual address within @vma
 *
 * Computes the correct physical frame number across large folio boundaries
 * and applies subpage slice offsets for 4KB compat mode.
 */
static inline pte_t vma_folio_mk_pte(const struct vm_area_struct *vma,
				     const struct folio *folio,
				     unsigned long addr)
{
	unsigned long pfn = folio_pfn(folio);

	if (vma->vm_file && folio_test_large(folio)) {
		pgoff_t pgoff = vma_linear_page_index(vma, addr);

		if (pgoff > folio->index)
			pfn += pgoff - folio->index;
	}

	return vma_pte_add_slice(vma, addr, pfn_pte(pfn, vma->vm_page_prot));
}

/*
 * p3s_pte_range_none - Check if a contiguous span of PTEs are all unmapped (none)
 * @pte: Starting PTE pointer
 * @nr_pages: Number of entries to inspect
 */
static inline bool p3s_pte_range_none(pte_t *pte, int nr_pages)
{
	int i;

	for (i = 0; i < nr_pages; i++) {
		if (!pte_none(ptep_get_lockless(pte + i)))
			return false;
	}

	return true;
}

/*
 * vma_folio_clamp_none_ptes - Clamp batch to contiguous empty PTEs
 * @vma: Pointer to struct vm_area_struct
 * @vmf: Fault information
 * @addr: In/Out starting virtual address of the slice batch
 * @nr_pages: In/Out number of pages in the slice batch
 *
 * In compatibility mode (e.g. 4KB process on 16KB host), an anonymous page
 * fault optimistically prepares a multi-page batch spanning up to an entire
 * host folio (P3S_SLICES_PER_PAGE = 4 slices). When subpage slices within
 * the same 16KB boundary have already been mapped by earlier faults (e.g.,
 * stack expansion during execve argument setup), the initial batch may
 * overlap existing PTEs.
 *
 * Under the page table lock, this helper clamps the batch to the contiguous
 * sub-range of empty (pte_none) PTEs that contains the faulting address
 * (@vmf->address).
 *
 * ┌──────┬──────┬──────┬──────┐
 * │ S0   │ S1   │ S2   │ S3   │
 * │ none │ none │ none │ mapped
 * └──────┴──────┴──────┴──────┘
 * ▲             ▲      ▲
 * └── *addr     │      └── pre-existing PTE
 *               └── vmf->address (fault)
 * ◄── clamped batch (nr = 3) ─►
 *
 * If the faulting PTE itself is already mapped (e.g. concurrent fault),
 * the batch is collapsed to nr_pages = 1 at vmf->address so standard
 * change detection triggers.
 */
static inline void vma_folio_clamp_none_ptes(const struct vm_area_struct *vma,
					     struct vm_fault *vmf,
					     unsigned long *addr,
					     int *nr_pages)
{
	unsigned int offset;
	int lo, hi;

	if (*nr_pages <= 1 || p3s_pte_range_none(vmf->pte, *nr_pages))
		return;

	offset = (vmf->address - *addr) >> mm_pte_shift(vma->vm_mm);
	if (!pte_none(ptep_get(vmf->pte + offset))) {
		vmf->pte += offset;
		*addr = vmf->address;
		*nr_pages = 1;
		return;
	}

	lo = offset;
	hi = offset + 1;

	while (lo > 0 && pte_none(ptep_get(vmf->pte + lo - 1)))
		lo--;
	while (hi < *nr_pages && pte_none(ptep_get(vmf->pte + hi)))
		hi++;

	vmf->pte += lo;
	*addr += (unsigned long)lo << mm_pte_shift(vma->vm_mm);
	*nr_pages = hi - lo;
}

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE

/*
 * p3s_anon_folio_lookaround_pte - Find adjacent anonymous folio in slice window
 * @vma: Pointer to struct vm_area_struct
 * @pte: Faulting PTE pointer
 * @addr: Faulting virtual address
 */
static inline struct folio *p3s_anon_folio_lookaround_pte(struct vm_area_struct *vma,
							  pte_t *pte,
							  unsigned long addr)
{
	unsigned int total_slices;
	unsigned long window_start;
	pte_t *pte_base;
	unsigned int i;

	if (!vma_is_p3s_4k(vma))
		return NULL;

	vma_folio_slice_bounds(vma, addr, &total_slices, &window_start);
	if (total_slices <= 1)
		return NULL;

	pte_base = pte - ((addr - window_start) >> PAGE_SHIFT_4KB);

	for (i = 0; i < total_slices; i++) {
		unsigned long slice_addr = window_start + (i << PAGE_SHIFT_4KB);
		pte_t ptent = ptep_get(pte_base + i);
		struct folio *f;

		if (!pte_present(ptent))
			continue;

		f = vm_normal_folio(vma, slice_addr, ptent);
		if (!f || !folio_test_anon(f) || folio_test_large(f) ||
		    folio_test_ksm(f))
			continue;

		if (!PageAnonExclusive(&f->page))
			continue;

		if (folio_try_get(f))
			return f;
	}

	return NULL;
}

/*
 * p3s_anon_folio_reuse - Update accounting when reusing an existing folio
 * @folio: Anonymous folio being reused
 * @vma: Pointer to struct vm_area_struct
 * @nr_pages: Number of pages being mapped
 */
static inline void p3s_anon_folio_reuse(struct folio *folio,
					struct vm_area_struct *vma,
					int nr_pages)
{
	if (nr_pages > 1)
		folio_ref_add(folio, nr_pages - 1);
	atomic_add(nr_pages, &folio->_mapcount);
	add_mm_counter(vma->vm_mm, MM_ANONPAGES, nr_pages);
}

/*
 * p3s_anon_install_folio - Look around for existing folio or install new anon folio
 * @vma: Pointer to struct vm_area_struct
 * @vmf: Fault information
 * @foliop: Pointer to folio pointer (updated if existing folio reused)
 * @addr: Faulting virtual address
 * @nr_pages: Number of subpage slices mapped
 * @entry: Pointer to PTE entry to populate
 */
static inline void p3s_anon_install_folio(struct vm_area_struct *vma,
					  struct vm_fault *vmf,
					  struct folio **foliop,
					  unsigned long addr,
					  int nr_pages,
					  pte_t *entry)
{
	struct folio *existing_folio = NULL;
	struct folio *folio = *foliop;

	if (likely(!userfaultfd_missing(vma)))
		existing_folio = p3s_anon_folio_lookaround_pte(vma, vmf->pte, addr);

	if (existing_folio) {
		folio_put(folio);
		*foliop = folio = existing_folio;
	}

	*entry = vma_folio_mk_pte(vma, folio, addr);
	*entry = pte_sw_mkyoung(*entry);
	if (vma->vm_flags & VM_WRITE)
		*entry = pte_mkwrite(pte_mkdirty(*entry), vma);

	if (existing_folio) {
		p3s_anon_folio_reuse(folio, vma, nr_pages);
		return;
	}

	folio_ref_add(folio, nr_pages - 1);
	add_mm_counter(vma->vm_mm, MM_ANONPAGES, nr_pages);
	count_mthp_stat(folio_order(folio), MTHP_STAT_ANON_FAULT_ALLOC);
	folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE);
	if (vma_is_p3s_4k(vma) && nr_pages > 1 && !folio_test_large(folio))
		atomic_add(nr_pages - 1, &folio->_mapcount);
	folio_add_lru_vma(folio, vma);
}

#ifdef CONFIG_USERFAULTFD
/*
 * p3s_uffd_install_anon_folio - Look around and reuse existing folio slice for userfaultfd
 * @dst_vma: Destination VMA
 * @dst_pte: Destination PTE pointer
 * @dst_addr: Destination virtual address
 * @folio: Newly allocated source folio
 * @dst_pte_val: Output PTE value to install
 * @writable: Whether the mapping should be writable
 * @flags: Userfaultfd atomic flags
 *
 * Returns true if an existing slice was reused and folio_put was called on @folio.
 */
static inline bool p3s_uffd_install_anon_folio(struct vm_area_struct *dst_vma,
					       pte_t *dst_pte,
					       unsigned long dst_addr,
					       struct folio *folio,
					       pte_t *dst_pte_val,
					       bool writable,
					       uffd_flags_t flags)
{
	struct folio *existing_folio =
		p3s_anon_folio_lookaround_pte(dst_vma, dst_pte, dst_addr);
	unsigned long offset;

	if (!existing_folio)
		return false;

	offset = (unsigned long)vma_slice_offset(dst_vma, dst_addr) << PAGE_SHIFT_4KB;

	memcpy(folio_address(existing_folio) + offset,
	       folio_address(folio) + offset, PAGE_SIZE_4KB);

	p3s_anon_folio_reuse(existing_folio, dst_vma, 1);
	*dst_pte_val = vma_folio_mk_pte(dst_vma, existing_folio, dst_addr);
	*dst_pte_val = pte_mkdirty(*dst_pte_val);
	if (writable)
		*dst_pte_val = pte_mkwrite(*dst_pte_val, dst_vma);
	if (flags & MFILL_ATOMIC_WP)
		*dst_pte_val = pte_mkuffd(*dst_pte_val);

	folio_put(folio);
	return true;
}
#endif /* CONFIG_USERFAULTFD */

/*
 * p3s_pte_maps_folio - Check if a PTE maps the given normal folio at addr
 * @vma: Pointer to struct vm_area_struct
 * @addr: Virtual address corresponding to @ptent
 * @ptent: Page table entry value
 * @folio: Expected folio pointer
 */
static inline bool p3s_pte_maps_folio(struct vm_area_struct *vma,
				      unsigned long addr, pte_t ptent,
				      const struct folio *folio)
{
	return pte_present(ptent) &&
	       vm_normal_folio(vma, addr, ptent) == folio;
}

/*
 * p3s_count_folio_slices - Count how many PTEs in a slice window map @folio
 * @vma: Pointer to struct vm_area_struct
 * @pte_base: Starting PTE pointer of the slice window
 * @start_addr: Starting virtual address of the slice window
 * @nr_slices: Number of slices in the window
 * @folio: Target folio to count
 */
static inline unsigned int p3s_count_folio_slices(struct vm_area_struct *vma,
						  pte_t *pte_base,
						  unsigned long start_addr,
						  unsigned int nr_slices,
						  const struct folio *folio)
{
	unsigned int i, count = 0;

	for (i = 0; i < nr_slices; i++) {
		if (p3s_pte_maps_folio(vma, start_addr + (i << PAGE_SHIFT_4KB),
				       ptep_get(pte_base + i), folio))
			count++;
	}
	return count;
}

/*
 * p3s_wp_can_reuse_anon_folio - Verify if anonymous folio can be reused on COW
 * @folio: Source anonymous folio
 * @vma: Pointer to struct vm_area_struct
 * @addr: Faulting virtual address
 * @pte: Faulting PTE pointer
 */
static inline bool p3s_wp_can_reuse_anon_folio(struct folio *folio,
					       struct vm_area_struct *vma,
					       unsigned long addr,
					       pte_t *pte)
{
	unsigned int nr_slices, local_slice_count;
	unsigned long start_addr;
	pte_t *pte_base;

	if (folio_test_large(folio) || folio_test_ksm(folio))
		return false;

	vma_folio_slice_bounds(vma, addr, &nr_slices, &start_addr);
	if (!nr_slices)
		return false;

	/*
	 * Early check: local slices cannot exceed total folio refcount.
	 * Max expected transient refcount is nr_slices + 1 (swapcache) + 1 (LRU).
	 */
	if (folio_ref_count(folio) > nr_slices + 2)
		return false;

	if (!folio_test_lru(folio))
		lru_add_drain();

	if (folio_ref_count(folio) > nr_slices + folio_test_swapcache(folio))
		return false;

	if (!folio_trylock(folio))
		return false;

	if (folio_test_swapcache(folio))
		folio_free_swap(folio);

	if (folio_test_ksm(folio)) {
		folio_unlock(folio);
		return false;
	}

	pte_base = pte - ((addr - start_addr) >> PAGE_SHIFT_4KB);
	local_slice_count = p3s_count_folio_slices(vma, pte_base, start_addr,
						   nr_slices, folio);

	if (local_slice_count == 0 ||
	    local_slice_count != folio_mapcount(folio) ||
	    local_slice_count != folio_ref_count(folio)) {
		folio_unlock(folio);
		return false;
	}

	SetPageAnonExclusive(&folio->page);
	folio_move_anon_rmap(folio, vma);
	folio_unlock(folio);
	return true;
}

/*
 * p3s_wp_slice_bounds - Check COW slice eligibility and compute slice bounds
 * @old_folio: Source shared folio
 * @vmf: Fault information
 * @nr_slices: Output number of slices
 * @start_addr: Output starting virtual address
 */
static inline bool p3s_wp_slice_bounds(struct folio *old_folio,
				       struct vm_fault *vmf,
				       unsigned int *nr_slices,
				       unsigned long *start_addr)
{
	struct vm_area_struct *vma = vmf->vma;

	if (!vma_is_p3s_4k(vma) || !old_folio ||
	    !folio_test_anon(old_folio) || folio_test_large(old_folio) ||
	    unlikely(userfaultfd_armed(vma)) || (vmf->flags & FAULT_FLAG_UNSHARE))
		return false;

	vma_folio_slice_bounds(vma, vmf->address, nr_slices, start_addr);
	return *nr_slices > 1;
}

/*
 * p3s_wp_copy_folio_slices - Copy multi-slice span to new folio during COW
 * @new_folio: Target freshly allocated folio
 * @old_folio: Source shared folio
 * @vmf: Fault information
 */
static inline int p3s_wp_copy_folio_slices(struct folio *new_folio,
					   struct folio *old_folio,
					   struct vm_fault *vmf)
{
	unsigned int nr_slices;
	unsigned long start_addr;

	if (!p3s_wp_slice_bounds(old_folio, vmf, &nr_slices, &start_addr))
		return -EINVAL;

	if (copy_mc_user_highpage(&new_folio->page, &old_folio->page, start_addr,
				  vmf->vma))
		return -EHWPOISON;

	return 0;
}

/*
 * p3s_wp_install_folio_slices - Install newly copied folio across slice span
 * @new_folio: Target freshly allocated folio
 * @old_folio: Source shared folio being replaced
 * @vmf: Fault information
 */
static inline bool p3s_wp_install_folio_slices(struct folio *new_folio,
					       struct folio *old_folio,
					       struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct mm_struct *mm = vma->vm_mm;
	unsigned int nr_slices, i;
	unsigned long start_addr;
	int mapped_slices;
	pte_t *pte_base;

	if (!p3s_wp_slice_bounds(old_folio, vmf, &nr_slices, &start_addr))
		return false;

	pte_base = vmf->pte - ((vmf->address - start_addr) >> PAGE_SHIFT_4KB);
	mapped_slices = p3s_count_folio_slices(vma, pte_base, start_addr,
					       nr_slices, old_folio);
	if (mapped_slices == 0)
		return false;

	folio_add_new_anon_rmap(new_folio, vma, start_addr, RMAP_EXCLUSIVE);
	if (mapped_slices > 1) {
		atomic_add(mapped_slices - 1, &new_folio->_mapcount);
		folio_ref_add(new_folio, mapped_slices - 1);
		folio_put_refs(old_folio, mapped_slices - 1);
	}
	folio_add_lru_vma(new_folio, vma);

	for (i = 0; i < nr_slices; i++) {
		unsigned long slice_addr = start_addr + (i << PAGE_SHIFT_4KB);
		pte_t *ptep = pte_base + i;
		pte_t ptent = ptep_get(ptep);
		pte_t entry;

		if (!p3s_pte_maps_folio(vma, slice_addr, ptent, old_folio))
			continue;

		flush_cache_page(vma, slice_addr, pte_pfn(ptent));
		ptep_clear_flush(vma, slice_addr, ptep);

		entry = vma_folio_mk_pte(vma, new_folio, slice_addr);
		entry = pte_sw_mkyoung(entry);
		if (pte_soft_dirty(ptent))
			entry = pte_mksoft_dirty(entry);
		if (pte_uffd(ptent))
			entry = pte_mkuffd(entry);
		entry = maybe_mkwrite(pte_mkdirty(entry), vma);

		set_pte_at(mm, slice_addr, ptep, entry);
		update_mmu_cache_range(vmf, vma, slice_addr, ptep, 1);

		folio_remove_rmap_pte(old_folio, &old_folio->page, vma);
	}

	return true;
}


/*
 * vma_anon_slice_window - First address of the slices of a page in @vma
 * @vma: 4KB compatibility COW VMA
 * @pgoff: Anonymous index of a small folio, in 4KB pages
 *
 * The slices of a small anonymous folio are mapped within one host-page
 * window, aligned to the slice phase of the VMA (the virtual address for
 * anonymous VMAs, the file slice for private file mappings). A VMA split or
 * a 4KB mremap() can leave some of them in a VMA whose page range does not
 * contain @pgoff; unsigned wrap-around then gives an address below vm_start,
 * and the slice arithmetic stays modular. Returns the first address of the
 * window within @vma, or -EFAULT if @vma holds no part of it.
 */
static inline unsigned long vma_anon_slice_window(const struct vm_area_struct *vma,
						  pgoff_t pgoff)
{
	unsigned long addr = vma->vm_start +
			     ((pgoff - vma_start_anon_pgoff(vma)) << PAGE_SHIFT_4KB);
	unsigned long win = addr -
			    ((unsigned long)vma_slice_offset(vma, addr) << PAGE_SHIFT_4KB);

	if (win >= vma->vm_end || win + PAGE_SIZE_KERNEL <= vma->vm_start)
		return -EFAULT;
	return max(win, vma->vm_start);
}

/*
 * vma_slice_walk_end - End of an rmap walk for a small folio from @addr
 * @vma: Pointer to struct vm_area_struct
 * @addr: Virtual address within @vma
 *
 * For 4KB compatibility VMAs, the end of the slice window at @addr within
 * @vma, so that the walk visits every slice PTE that can map the folio.
 * One page past @addr for other VMAs.
 */
static inline unsigned long vma_slice_walk_end(const struct vm_area_struct *vma,
					       unsigned long addr)
{
	unsigned long off;

	if (!vma_is_p3s_4k(vma))
		return addr + vma_page_size(vma);

	off = (unsigned long)vma_slice_offset(vma, addr) << PAGE_SHIFT_4KB;
	addr &= PAGE_MASK_4KB;
	return addr + min(PAGE_SIZE_KERNEL - off, vma->vm_end - addr);
}

/*
 * p3s_anon_rmap_range - Widen the anon_vma lookup of @folio to its window
 * @folio: Folio being walked
 * @start: First page offset to look up
 * @end: Last page offset to look up
 *
 * A small folio in a 4KB VMA can be mapped from the four slices of one
 * host-page window, and a VMA split or a 4KB mremap() can leave some of them
 * in a VMA whose page range does not contain the folio's index. Look up
 * every page offset of the window.
 */
static inline void p3s_anon_rmap_range(const struct folio *folio,
				       pgoff_t *start, pgoff_t *end)
{
	if (folio_test_large(folio))
		return;
	*start -= min_t(pgoff_t, *start, P3S_SLICE_MASK);
	*end += P3S_SLICE_MASK;
}

/*
 * p3s_anon_rmap_skip - Check whether an anon rmap walk of @folio skips @vma
 * @vma: VMA found by the lookup that p3s_anon_rmap_range() widened
 * @folio: Folio being walked
 * @address: vma_anon_address() of @folio in @vma; the start of the walk on
 *           return
 *
 * In a 4KB VMA the walk starts at the first address of @folio's window in
 * @vma. Returns true for a VMA that holds no part of the window, which the
 * widened lookup can find in any mm, and for which vma_anon_address() of a
 * native VMA failed.
 */
static inline bool p3s_anon_rmap_skip(const struct vm_area_struct *vma,
				      const struct folio *folio,
				      unsigned long *address)
{
	if (folio_test_large(folio))
		return false;
	if (vma_is_p3s_4k(vma))
		*address = vma_anon_slice_window(vma, folio_pgoff(folio));
	return *address == -EFAULT;
}

/*
 * p3s_folio_mapped_by_slices - Check if other slice PTEs may map @folio
 * @vma: Pointer to struct vm_area_struct
 * @folio: Folio mapped by a PTE of @vma
 *
 * True for a small folio of a 4KB VMA with more than one mapping. Its slices
 * share one swap entry, so none of their swap PTEs may be exclusive.
 */
static inline bool p3s_folio_mapped_by_slices(const struct vm_area_struct *vma,
					      const struct folio *folio)
{
	return vma_is_p3s_4k(vma) && !folio_test_large(folio) &&
	       folio_mapcount(folio) > 1;
}

/*
 * p3s_folio_slices_in_window - Check that all mappings of @folio are here
 * @vma: Pointer to struct vm_area_struct
 * @folio: Folio about to be unmapped by reclaim or migration
 * @addr: Address of a PTE in @vma that maps @folio
 * @pte: That PTE, mapped and locked
 *
 * A 4KB compatibility task can map a small anonymous folio with several
 * slice PTEs, all covered by its one PageAnonExclusive flag. Unmapping the
 * first of them clears the flag (folio_try_share_anon_rmap_pte()), so the
 * other slices must be unmapped in the same walk: a slice left mapped
 * writable would reference a page that is no longer exclusive. Returns
 * true if every mapping of @folio is a PTE in the slice window at @addr,
 * which the walk covers under the page table lock it already holds.
 */
static inline bool p3s_folio_slices_in_window(struct vm_area_struct *vma,
					      struct folio *folio,
					      unsigned long addr, pte_t *pte)
{
	unsigned int nr_slices;
	unsigned long start;

	if (!p3s_folio_mapped_by_slices(vma, folio))
		return true;

	vma_folio_slice_bounds(vma, addr, &nr_slices, &start);
	return p3s_count_folio_slices(vma,
				      pte - ((addr - start) >> PAGE_SHIFT_4KB),
				      start, nr_slices, folio) ==
	       folio_mapcount(folio);
}

/*
 * vma_folio_nr_ptes - Number of PTEs that map all of @folio in @vma
 * @vma: Pointer to struct vm_area_struct
 * @folio: Folio mapped by @vma
 *
 * The rmap walks mlock a large folio only once they have counted this many
 * PTEs in a VM_LOCKED VMA. A 4KB VMA maps each page of the folio with four
 * PTEs, one per slice. A small folio is still mlocked on its first PTE.
 */
static inline unsigned long vma_folio_nr_ptes(const struct vm_area_struct *vma,
					      const struct folio *folio)
{
	if (vma_is_p3s_4k(vma) && folio_test_large(folio))
		return folio_nr_pages(folio) << P3S_SLICE_SHIFT;
	return folio_nr_pages(folio);
}

/*
 * vma_anon_index_matches - Check an anonymous page's index against @addr
 * @vma: Pointer to struct vm_area_struct
 * @pgoff: Anonymous index of the page (page_pgoff())
 * @addr: Virtual address the page is mapped at
 *
 * In a 4KB VMA, the index of a small folio is that of the slice that faulted
 * first, and its other slices are mapped in the same window, so compare the
 * windows. Elsewhere the index must match exactly.
 */
static inline bool vma_anon_index_matches(const struct vm_area_struct *vma,
					  pgoff_t pgoff, unsigned long addr)
{
	unsigned long first;

	if (!vma_is_p3s_4k(vma))
		return pgoff == linear_anon_page_index(vma, addr);

	first = vma->vm_start +
		((pgoff - vma_start_anon_pgoff(vma)) << PAGE_SHIFT_4KB);
	addr &= PAGE_MASK_4KB;
	return first - ((unsigned long)vma_slice_offset(vma, first) << PAGE_SHIFT_4KB) ==
	       addr - ((unsigned long)vma_slice_offset(vma, addr) << PAGE_SHIFT_4KB);
}

#else /* !CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */

static inline unsigned long vma_slice_walk_end(const struct vm_area_struct *vma,
					       unsigned long addr)
{
	return addr + PAGE_SIZE;
}

static inline void p3s_anon_rmap_range(const struct folio *folio,
				       pgoff_t *start, pgoff_t *end)
{
}

static inline bool p3s_anon_rmap_skip(const struct vm_area_struct *vma,
				      const struct folio *folio,
				      unsigned long *address)
{
	return *address == -EFAULT;
}

static inline bool p3s_folio_mapped_by_slices(const struct vm_area_struct *vma,
					      const struct folio *folio)
{
	return false;
}

static inline bool p3s_folio_slices_in_window(struct vm_area_struct *vma,
					      struct folio *folio,
					      unsigned long addr, pte_t *pte)
{
	return true;
}

static inline unsigned long vma_folio_nr_ptes(const struct vm_area_struct *vma,
					      const struct folio *folio)
{
	return folio_nr_pages(folio);
}

static inline bool vma_anon_index_matches(const struct vm_area_struct *vma,
					  pgoff_t pgoff, unsigned long addr)
{
	return pgoff == linear_anon_page_index(vma, addr);
}

static inline void p3s_anon_install_folio(struct vm_area_struct *vma,
					  struct vm_fault *vmf,
					  struct folio **foliop,
					  unsigned long addr,
					  int nr_pages,
					  pte_t *entry)
{
	struct folio *folio = *foliop;

	*entry = folio_mk_pte(folio, vma->vm_page_prot);
	*entry = pte_sw_mkyoung(*entry);
	if (vma->vm_flags & VM_WRITE)
		*entry = pte_mkwrite(pte_mkdirty(*entry), vma);

	folio_ref_add(folio, nr_pages - 1);
	add_mm_counter(vma->vm_mm, MM_ANONPAGES, nr_pages);
	count_mthp_stat(folio_order(folio), MTHP_STAT_ANON_FAULT_ALLOC);
	folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE);
	folio_add_lru_vma(folio, vma);
}

#ifdef CONFIG_USERFAULTFD
static inline bool p3s_uffd_install_anon_folio(struct vm_area_struct *dst_vma,
					       pte_t *dst_pte,
					       unsigned long dst_addr,
					       struct folio *folio,
					       pte_t *dst_pte_val,
					       bool writable,
					       uffd_flags_t flags)
{
	return false;
}
#endif /* CONFIG_USERFAULTFD */

static inline bool p3s_wp_can_reuse_anon_folio(struct folio *folio,
					       struct vm_area_struct *vma,
					       unsigned long addr,
					       pte_t *pte)
{
	return false;
}

static inline int p3s_wp_copy_folio_slices(struct folio *new_folio,
					   struct folio *old_folio,
					   struct vm_fault *vmf)
{
	return -EINVAL;
}

static inline bool p3s_wp_install_folio_slices(struct folio *new_folio,
					       struct folio *old_folio,
					       struct vm_fault *vmf)
{
	return false;
}

#endif /* CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */

#endif /* __MM_P3S_H */
