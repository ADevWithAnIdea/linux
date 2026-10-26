/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_MREMAP_H
#define _LINUX_P3S_MREMAP_H

#include <linux/p3s/vma.h>
#include <linux/err.h>

#ifndef MREMAP_FIXED
#define MREMAP_FIXED 2
#endif

struct file;
unsigned long __get_unmapped_area(struct file *file, unsigned long addr,
				  unsigned long len, unsigned long pgoff,
				  unsigned long flags, vma_flags_t vma_flags);

/**
 * mm_mremap_get_unmapped_area - Find unmapped area for mremap with slice preservation
 * @file: Backing file (if any)
 * @addr: Target address / user hint
 * @len: Length of the new mapping
 * @pgoff: Page offset in the file / mapping
 * @map_flags: MAP_* flags for get_unmapped_area
 * @old_addr: Source address being remapped
 * @mremap_flags: MREMAP_* flags (e.g. MREMAP_FIXED, MREMAP_DONTUNMAP)
 *
 * For 4KB compat processes on a 16KB kernel, a VMA may start at a non-zero
 * subpage slice offset within a host 16KB folio ((old_addr >> 12) & 3 != 0).
 *
 * When mremap() moves a VMA without MREMAP_FIXED, the destination address
 * must preserve the identical subpage slice offset within its enclosing 16KB
 * boundary so that page cache indexing, physical page alignment, and subpage
 * slicing remain consistent between old and new mappings:
 *
 * ┌─────────────────────────────────────────────────────────┐
 * │ 16KB Host Folio                                         │
 * ├─────────────┬─────────────┬─────────────┬───────────────┤
 * │   Slice 0   │   Slice 1   │   Slice 2   │    Slice 3    │
 * └─────────────┴─────────────┴─────────────┴───────────────┘
 *               ▲
 *               └── old_addr starts at Slice 1
 *                   relocated dest is padded and offset to Slice 1
 *
 * When MREMAP_FIXED is specified, move_ptes() transfers raw 4KB PTEs whose
 * physical frame bits already encode the source subpage slice ((old_addr >> 12) & 3),
 * while subsequent COW, migration, sibling lookaround, and file faults compute
 * the slice index from the destination virtual address (vma_slice_offset()).
 * Because mremap() must preserve physical PFNs (copying to a new folio would
 * corrupt concurrent get_user_pages() / DMA pins), MREMAP_FIXED requests with
 * mismatched modulo-16KB slice offsets (((old_addr ^ addr) & ~PAGE_MASK_KERNEL) != 0)
 * are rejected with -EINVAL.
 *
 * Android ART's MREMAP_FIXED users (MarkCompact::KernelPrepareRangeForUffd
 * which aligns heap/shadow ranges to >= 2 MiB PMD boundaries, and
 * MemMap::ReplaceWith which operates on top-level 16KB-aligned mmap() base
 * addresses) always satisfy (old_addr ^ addr) & ~PAGE_MASK_KERNEL == 0.
 */
static inline unsigned long mm_mremap_get_unmapped_area(struct file *file,
		unsigned long addr, unsigned long len, unsigned long pgoff,
		unsigned long map_flags, unsigned long old_addr,
		unsigned long mremap_flags)
{
	if (mm_is_p3s_4k(current->mm)) {
		unsigned int slice =
			(old_addr >> PAGE_SHIFT_4KB) & P3S_SLICE_MASK;
		bool same_slice = !((old_addr ^ addr) & ~PAGE_MASK_KERNEL);

		if (mremap_flags & MREMAP_FIXED) {
			if (!same_slice)
				return -EINVAL;
		} else if (slice || (addr && !same_slice)) {
			unsigned long res;

			/*
			 * MREMAP_DONTUNMAP treats a non-zero destination as an
			 * mmap-style hint. Honor an available hint at the
			 * source slice before adding padding to preserve the
			 * source address slice.
			 */
			if (addr && same_slice) {
				res = __get_unmapped_area(file, addr, len,
							  pgoff, map_flags, EMPTY_VMA_FLAGS);
				if (res == addr || IS_ERR_VALUE(res))
					return res;
			}

			res = __get_unmapped_area(file, addr,
						  len + PAGE_SIZE_KERNEL,
						  pgoff, map_flags, EMPTY_VMA_FLAGS);
			/* The first address in the padded area at the source slice */
			if (!IS_ERR_VALUE(res))
				res += (old_addr - res) & ~PAGE_MASK_KERNEL;
			return res;
		}
	}

	return __get_unmapped_area(file, addr, len, pgoff, map_flags, EMPTY_VMA_FLAGS);
}

#endif /* _LINUX_P3S_MREMAP_H */
