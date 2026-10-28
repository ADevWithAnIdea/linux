// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kstrtox.h>
#include <linux/mm_types.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/p3s/mm.h>
#include <linux/p3s/vma.h>
#include <linux/leafops.h>
#include "vma.h"

enum p3s_4k_mode p3s_4k_mode __ro_after_init = P3S_4K_MODE_OFF;
EXPORT_SYMBOL(p3s_4k_mode);

static int __init parse_p3s_4k(char *str)
{
	bool enabled;

	if (!str)
		return -EINVAL;

	if (!kstrtobool(str, &enabled)) {
		p3s_4k_mode = enabled ? P3S_4K_MODE_ON : P3S_4K_MODE_OFF;
		return 0;
	}

	if (!strcmp(str, "alternate")) {
		p3s_4k_mode = P3S_4K_MODE_ALTERNATE;
		return 0;
	}

	return -EINVAL;
}
early_param("p3s_4k", parse_p3s_4k);

/*
 * vmg_can_merge_offsets - Verify offset & slice continuity for VMA merge
 * @vmg: VMA merge structure
 * @merge_next: True if merging with vmg->next, false if merging with vmg->prev
 */
bool vmg_can_merge_offsets(struct vma_merge_struct *vmg, bool merge_next)
{
	if (merge_next) {
		unsigned int start_slice;
		unsigned long total_slices;

		if (vmg->middle)
			return vma_can_merge_offsets(vmg->middle,
						     vmg->next->vm_pgoff,
						     vma_slice_off(vmg->next),
						     vmg->end,
						     vmg_is_p3s_4k(vmg),
						     !!vmg->file);

		if (!vmg_is_p3s_4k(vmg))
			return vmg->next->vm_pgoff ==
			       vmg->pgoff + PHYS_PFN(vmg->end - vmg->start);

		if (!vmg->file)
			return vmg->next->vm_pgoff ==
			       vmg->pgoff + ((vmg->end - vmg->start) >> PAGE_SHIFT_4KB);

		/* File and virtual addresses need not have the same 16KB offset. */
		start_slice = vmg->slice_off;
		total_slices = start_slice + ((vmg->end - vmg->start) >> PAGE_SHIFT_4KB);

		return vmg->next->vm_pgoff == vmg->pgoff + (total_slices >> P3S_SLICE_SHIFT) &&
		       vma_slice_off(vmg->next) == (total_slices & P3S_SLICE_MASK);
	}

	if (!vmg->prev)
		return false;

	return vma_can_merge_offsets(vmg->prev, vmg->pgoff,
				     vmg->middle ? vma_slice_offset(vmg->middle, vmg->start) :
				     vmg->slice_off,
				     vmg->start, vmg_is_p3s_4k(vmg), !!vmg->file);
}

/* OOM weights are native pages, not logical RSS slices. */
struct p3s_backing {
	unsigned long index;
	enum { P3S_NONE, P3S_RESIDENT, P3S_SWAP } kind;
};

static struct p3s_backing p3s_pte_backing(pte_t pte)
{
	struct p3s_backing b = { .kind = P3S_NONE };
	softleaf_t entry;

	if (pte_present(pte)) {
		if (!pte_special(pte)) {
			b.kind = P3S_RESIDENT;
			b.index = pte_pfn(pte);
		}
	} else if (!pte_none(pte)) {
		entry = softleaf_from_pte(pte);
		if (softleaf_is_migration(entry)) {
			b.kind = P3S_RESIDENT;
			b.index = softleaf_to_pfn(entry);
		} else if (softleaf_is_swap(entry)) {
			b.kind = P3S_SWAP;
			b.index = entry.val;
		}
	}
	return b;
}

static bool p3s_same_backing(struct p3s_backing a, struct p3s_backing b)
{
	return a.kind == b.kind && (a.kind == P3S_NONE || a.index == b.index);
}

/*
 * The caller owns the PTE lock (or an unpublished page table). All four slots
 * of a native-size VA window share that lock and cannot cross a PTE table.
 * Count each backing PFN/swap slot once in that window. Sparse slices then
 * retain the full allocation cost, while dense siblings do not count it x4.
 * Aliases in different windows still count repeatedly, as native RSS does.
 * An incongruent file mapping can overcount a boundary page; this is a bounded
 * conservative OOM heuristic, not PSS or a unique per-mm physical census.
 * No allocation, mmap lock, page walk, rmap or extra page reference is needed.
 */
void p3s_account_pte(struct mm_struct *mm, unsigned long addr, pte_t *ptep,
		     pte_t old, pte_t new)
{
	struct p3s_backing before = p3s_pte_backing(old);
	struct p3s_backing after = p3s_pte_backing(new);
	unsigned int slot = ((unsigned long)ptep / sizeof(pte_t)) &
			    (P3S_SLICES_PER_PAGE - 1);
	bool lost = before.kind != P3S_NONE, added = after.kind != P3S_NONE;
	unsigned int i;

	if (p3s_same_backing(before, after))
		return;
	/* A mismatched VA must never move the scan outside its PTE table. */
	VM_WARN_ON_ONCE(slot !=
		       ((addr & (PAGE_SIZE_KERNEL - 1)) >> PAGE_SHIFT_4KB));
	ptep -= slot;
	for (i = 0; i < P3S_SLICES_PER_PAGE; i++) {
		struct p3s_backing sibling;

		if (i == slot)
			continue;
		sibling = p3s_pte_backing(READ_ONCE(ptep[i]));
		if (p3s_same_backing(before, sibling))
			lost = false;
		if (p3s_same_backing(after, sibling))
			added = false;
	}
	if (added != lost)
		atomic_long_add((int)added - (int)lost, &mm->p3s_oom_pages);
}
EXPORT_SYMBOL_GPL(p3s_account_pte);
