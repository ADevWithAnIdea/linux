/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_H
#define _LINUX_P3S_H

#include <linux/p3s/const.h>
#include <linux/p3s/mm.h>
#include <linux/p3s/vma.h>
#include <linux/p3s/mremap.h>

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
long p3s_get_user_page(struct mm_struct *mm, unsigned long addr,
		      unsigned int gup_flags, bool pin, bool remote,
		      struct page **page, unsigned long *offset);
#else
static inline long p3s_get_user_page(struct mm_struct *mm, unsigned long addr,
		unsigned int gup_flags, bool pin, bool remote,
		struct page **page, unsigned long *offset)
{
	return -EINVAL;
}
#endif

/* asm/p3s.h is architecture-specific to arm64 */
#ifdef CONFIG_ARM64
#include <asm/p3s/pgtable.h>
#include <asm/p3s/mmu.h>
#include <asm/p3s.h>
#else
#define mm_pmd_offset(mm, pud, addr)		pmd_offset(pud, addr)
#define mm_pte_offset_kernel(mm, pmd, addr)	pte_offset_kernel(pmd, addr)
#define pte_advance_phys(pte, bytes)		(pte)
#define __pte_to_phys(pte)			((phys_addr_t)pte_pfn(pte) << PAGE_SHIFT_KERNEL)
#ifndef __ASSEMBLY__
#define P3S_CONTEXT_REMOTE_MM(remote_mm)	do { } while (0)
static inline struct mm_struct *p3s_current_mm(void)
{
	return current ? current->mm : NULL;
}
static inline void p3s_assert_matching_pte_shift(const struct mm_struct *mm)
{
}
#endif /* !__ASSEMBLY__ */
#endif /* CONFIG_ARM64 */

#endif /* _LINUX_P3S_H */
