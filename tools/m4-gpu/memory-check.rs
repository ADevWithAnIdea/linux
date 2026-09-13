// SPDX-License-Identifier: GPL-2.0-only
// Copyright The Gravity Linux Contributors
// Host stand-ins for the actual driver methods loaded by check-memory.py.
use std::{
    cell::{Cell, RefCell, UnsafeCell},
    collections::BTreeMap,
    mem::size_of,
    ops::{Deref, DerefMut, Range},
};
type Result<T = ()> = std::result::Result<T, i32>;
type PhysicalAddr = u64;
type Owned<T> = Box<T>;
const GFP_KERNEL: u32 = 0;
const __GFP_ZERO: u32 = 1;
const EINVAL: i32 = 22;
const EIO: i32 = 5;
const EEXIST: i32 = 17;
const ENOMEM: i32 = 12;
const PAGE: usize = 0x4000;
macro_rules! pr_debug {
    ($($arg:tt)*) => {};
}
macro_rules! pr_err {
    ($($arg:tt)*) => {};
}
thread_local! {
    static PAGES: RefCell<BTreeMap<u64, usize>> = const { RefCell::new(BTreeMap::new()) };
    static NEXT: Cell<u64> = const { Cell::new(PAGE as u64) };
    static CLEAN: Cell<usize> = const { Cell::new(0) };
    static INVALIDATE: Cell<usize> = const { Cell::new(0) };
    static BARRIERS: Cell<usize> = const { Cell::new(0) };
    static PUSHES: Cell<usize> = const { Cell::new(0) };
    static FAIL_PUSH: Cell<Option<usize>> = const { Cell::new(None) };
}
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn new() -> Self {
        Self(Vec::new())
    }
    fn reserve(&mut self, n: usize, _: u32) -> Result { self.0.reserve(n); Ok(()) }
    fn push(&mut self, item: T, _: u32) -> Result {
        PUSHES.set(PUSHES.get() + 1);
        if let Some(left) = FAIL_PUSH.get() {
            if left == 0 {
                FAIL_PUSH.set(None);
                return Err(ENOMEM);
            }
            FAIL_PUSH.set(Some(left - 1));
        }
        self.0.push(item);
        Ok(())
    }
    fn extend_with(&mut self, n: usize, item: T, _: u32) -> Result
    where
        T: Clone,
    {
        self.0.resize(self.0.len() + n, item);
        Ok(())
    }
}
impl<T> Deref for KVec<T> {
    type Target = Vec<T>;
    fn deref(&self) -> &Vec<T> {
        &self.0
    }
}
impl<T> DerefMut for KVec<T> {
    fn deref_mut(&mut self) -> &mut Vec<T> {
        &mut self.0
    }
}
impl<'a, T> IntoIterator for &'a KVec<T> {
    type Item = &'a T;
    type IntoIter = std::slice::Iter<'a, T>;
    fn into_iter(self) -> Self::IntoIter {
        self.0.iter()
    }
}
struct Page {
    phys: u64,
    cpu: UnsafeCell<Box<[u64; PAGE / 8]>>,
    device: UnsafeCell<Box<[u8; PAGE]>>,
}
impl Page {
    fn alloc_page(_: u32) -> Result<Box<Self>> {
        let phys = NEXT.get();
        NEXT.set(phys + PAGE as u64);
        let page = Box::new(Self {
            phys,
            cpu: UnsafeCell::new(Box::new([0; PAGE / 8])),
            device: UnsafeCell::new(Box::new([0xcc; PAGE])),
        });
        PAGES.with_borrow_mut(|p| p.insert(phys, &*page as *const Self as usize));
        Ok(page)
    }
    fn phys(&self) -> u64 {
        self.phys
    }
    fn into_phys(page: Box<Self>) -> u64 {
        let p = page.phys;
        let _ = Box::into_raw(page);
        p
    }
    unsafe fn from_phys(phys: u64) -> Box<Self> {
        Box::from_raw(PAGES.with_borrow(|p| p[&phys]) as *mut Self)
    }
    unsafe fn borrow_phys(phys: &u64) -> Option<&Self> {
        PAGES.with_borrow(|p| p.get(phys).map(|p| &*(*p as *const Self)))
    }
    unsafe fn borrow_phys_unchecked(phys: &u64) -> &Self {
        Self::borrow_phys(phys).unwrap()
    }
    fn ptr(&self) -> *mut u8 {
        unsafe { (*self.cpu.get()).as_mut_ptr().cast() }
    }
    fn with_page_mapped<T>(&self, f: impl FnOnce(*mut u8) -> T) -> T {
        f(self.ptr())
    }
    fn with_pointer_into_page<T>(
        &self,
        offset: usize,
        size: usize,
        f: impl FnOnce(*mut u8) -> Result<T>,
    ) -> Result<T> {
        if offset.checked_add(size).is_none_or(|e| e > PAGE) {
            return Err(EINVAL);
        }
        f(unsafe { self.ptr().add(offset) })
    }
    fn device_write(&self, offset: usize, bytes: &[u8]) {
        unsafe { (&mut **self.device.get())[offset..offset + bytes.len()].copy_from_slice(bytes) }
    }
    fn device_bytes(&self) -> Vec<u8> {
        unsafe { (&**self.device.get()).to_vec() }
    }
}
impl Drop for Page {
    fn drop(&mut self) {
        PAGES.with_borrow_mut(|p| p.remove(&self.phys));
    }
}
mod mem {
    pub fn sync() {
        super::BARRIERS.set(super::BARRIERS.get() + 1)
    }
    pub fn tlbi_all() {}
}
mod util {
    pub fn align(a: u64, b: u64) -> u64 {
        (a + b - 1) & !(b - 1)
    }
}
mod cache {
    use super::*;
    unsafe fn line(p: *mut u8) -> (&'static Page, usize) {
        PAGES.with_borrow(|pages| {
            pages
                .values()
                .find_map(|v| {
                    let page = &*(*v as *const Page);
                    let base = page.ptr() as usize;
                    let addr = p as usize;
                    (addr >= base && addr < base + PAGE).then(|| (page, (addr - base) & !63))
                })
                .unwrap()
        })
    }
    pub unsafe fn clean(p: *mut u8) {
        CLEAN.set(CLEAN.get() + 1);
        let (page, off) = line(p);
        std::ptr::copy_nonoverlapping(
            page.ptr().add(off),
            (*page.device.get()).as_mut_ptr().add(off),
            64,
        );
    }
    pub unsafe fn invalidate(p: *mut u8) {
        INVALIDATE.set(INVALIDATE.get() + 1);
        let (page, off) = line(p);
        std::ptr::copy_nonoverlapping(
            (*page.device.get()).as_ptr().add(off),
            page.ptr().add(off),
            64,
        );
    }
}
mod pgtable {
    include!("pgtable.rs");
}
mod vm {
    include!("vm.rs");
    fn image(space: &FirmwareSpace) -> Vec<u8> {
        space
            .regions
            .iter()
            .flat_map(|r| r.pages.iter().flat_map(|p| p.device_bytes()))
            .collect()
    }
    pub fn check() {
        let base = 0x40000;
        let mut space = FirmwareSpace {
            table: UatPageTable::new_with_ias(42, 42).unwrap(),
            regions: KVec::new(),
            external: KVec::new(),
        };
        space
            .alloc(base, 3 * PAGE, pgtable::prot::PROT_FW_PRIV_RW)
            .unwrap();
        space.write(base + 4, b"init").unwrap();
        space
            .init_page(base + PAGE as u64, |p| {
                p[77] = 0x12;
                Ok(())
            })
            .unwrap();
        reset();
        space.sync();
        assert_eq!(CLEAN.get(), 6 * (PAGE / 64));
        assert_eq!(BARRIERS.get(), 2);
        reset();
        space.sync();
        assert_eq!(CLEAN.get(), 0);
        // Firmware updates neighbors in the same cache lines as a host write.
        space.regions[0].pages[0].device_write(PAGE - 8, &[0x5a; 5]);
        space.regions[0].pages[2].device_write(8, &[0xa5; 7]);
        let mut expected = image(&space);
        let bytes: Vec<_> = (0..PAGE + 8).map(|i| (i % 251) as u8).collect();
        expected[PAGE - 3..2 * PAGE + 5].copy_from_slice(&bytes);
        reset();
        space.write_live(base + PAGE as u64 - 3, &bytes).unwrap();
        assert_eq!(image(&space), expected);
        assert_eq!(BARRIERS.get(), 2);
        assert_eq!(CLEAN.get(), PAGE / 64 + 2);
        assert_eq!(INVALIDATE.get(), PAGE / 64 + 2);
        expected[9..PAGE + 24].fill(0);
        reset();
        space.zero_live(base + 9, PAGE + 15).unwrap();
        assert_eq!(image(&space), expected);
        assert_eq!(BARRIERS.get(), 2);
        reset();
        space.sync();
        assert_eq!(CLEAN.get(), 0);
        // Live access before publication must retain dirty CPU initialization.
        let next = 0x80000;
        space
            .alloc(next, 2 * PAGE, pgtable::prot::PROT_FW_PRIV_RW)
            .unwrap();
        space.write(next + 5, &[0xab]).unwrap();
        reset();
        space.write_live(next + 7, &[0xcd]).unwrap();
        assert_eq!(INVALIDATE.get(), 0);
        assert_eq!(CLEAN.get(), PAGE / 64);
        let page = &space.regions[1].pages[0];
        let data = page.device_bytes();
        assert_eq!((data[5], data[7], data[6]), (0xab, 0xcd, 0));
        reset();
        space.sync();
        assert_eq!(CLEAN.get(), 2 * PAGE / 64); // Other data page + edited leaf table.
        reset();
        space.sync();
        assert_eq!(CLEAN.get(), 0);
        space.write(next + PAGE as u64 + 17, &[0x98]).unwrap();
        reset();
        space.sync();
        assert_eq!(CLEAN.get(), PAGE / 64);
        assert_eq!(
            space.init_page(next, |p| {
                p[41] = 0x67;
                Err(EINVAL)
            }),
            Err(EINVAL)
        );
        reset();
        space.sync();
        assert_eq!(CLEAN.get(), PAGE / 64);
        assert_eq!(space.regions[1].pages[0].device_bytes()[41], 0x67);
        let before = image(&space);
        reset();
        assert_eq!(space.write_live(u64::MAX - 3, &[1; 8]), Err(EINVAL));
        assert_eq!(space.zero_live(base + 3 * PAGE as u64 - 2, 8), Err(EINVAL));
        space.write_live(base, &[]).unwrap();
        assert_eq!(image(&space), before);
        assert_eq!(BARRIERS.get(), 0);
        println!("PASS firmware memory: dirty-only cleaning, three-page writes, two-page zeroing, firmware neighbors, initialization, errors, two barriers per batch");
    }
}
fn reset() {
    CLEAN.set(0);
    INVALIDATE.set(0);
    BARRIERS.set(0)
}
fn check_tables() {
    use pgtable::{prot::PROT_GPU_SHARED_RW as RW, UatPageTable};
    let page = PAGE as u64;
    let far = 1u64 << 36;
    let mut table = UatPageTable::new_with_ias(42, 42).unwrap();
    table
        .map_pages(page..3 * page, 0x9000000, RW, false)
        .unwrap();
    reset();
    table.sync();
    assert_eq!(CLEAN.get(), 3 * PAGE / 64);
    assert_eq!(BARRIERS.get(), 1);
    reset();
    let pushes = PUSHES.get();
    assert_eq!(table.translate(page + 1).unwrap(), Some(0x9000001));
    let leaf = table.leaf(page).unwrap();
    assert_eq!(table.translate(far).unwrap(), None);
    assert_eq!(PUSHES.get(), pushes);
    table.sync();
    assert_eq!(CLEAN.get(), 0);
    table.unmap_pages(page..2 * page).unwrap();
    reset();
    table.sync();
    assert_eq!(CLEAN.get(), PAGE / 64);
    table.restore_leaf(page, leaf).unwrap();
    reset();
    table.sync();
    assert_eq!(CLEAN.get(), PAGE / 64);
    reset();
    table.sync();
    assert_eq!(CLEAN.get(), 0);
    {
        let mut borrowed =
            UatPageTable::new_with_ttb_and_ias(table.ttb(), 0..1 << 42, 42, 42).unwrap();
        borrowed
            .map_pages(far..far + page, 0x9100000, RW, false)
            .unwrap();
        reset();
        borrowed.sync();
        assert_eq!(CLEAN.get(), 3 * PAGE / 64);
        assert_eq!(borrowed.translate(page).unwrap(), Some(0x9000000));
    }
    assert_eq!(table.translate(far).unwrap(), None);
    assert_eq!(table.translate(page).unwrap(), Some(0x9000000));
    println!("PASS page tables: read-only walks allocate/clean nothing, edited leaves clean once, new zero tables and borrowed ancestors publish, borrowed teardown preserves mappings");
}
fn check_allocation_errors() {
    use pgtable::{prot::PROT_GPU_SHARED_RW as RW, UatPageTable};
    for fail_after in 0..4 {
        let mut table = UatPageTable::new_with_ias(42, 42).unwrap();
        table.sync();
        FAIL_PUSH.set(Some(fail_after));
        assert_eq!(
            table.map_pages(0x4000..0x8000, 0x9000000, RW, false),
            Err(ENOMEM)
        );
        FAIL_PUSH.set(None);
        table.sync(); // Every pending cache clean still targets a live table.
        assert_eq!(table.translate(0x4000).unwrap(), None);
        table
            .map_pages(0x4000..0x8000, 0x9000000, RW, false)
            .unwrap();
        table.sync();
        assert_eq!(table.translate(0x4000).unwrap(), Some(0x9000000));
        drop(table);
        assert!(PAGES.with_borrow(|p| p.is_empty()));
    }
    println!("PASS allocation errors: dirty/ownership metadata failures retain valid pages, permit retry, and release all table ownership");
}
fn check_forks() {
    use pgtable::{prot::{PROT_GPU_SHARED_RW as RW, PROT_GPU_SHARED_RO as RO}, UatPageTable};
    let mut source = UatPageTable::new_with_ias(42, 42).unwrap();
    let addresses = [0x4000u64, 0x2000000, 1 << 36, 1 << 40];
    for (i, &address) in addresses.iter().enumerate() {
        source.map_pages(address..address + PAGE as u64, 0x9000000 + i as u64 * PAGE as u64,
            if i & 1 == 0 { RW } else { RO }, false).unwrap();
    }
    source.sync();
    let page_count = PAGES.with_borrow(|p| p.len());
    let mut succeeded = false;
    for fail_after in 0..32 {
        FAIL_PUSH.set(Some(fail_after));
        let copied = source.fork();
        FAIL_PUSH.set(None);
        match copied {
            Err(ENOMEM) => (),
            Err(error) => panic!("unexpected fork error {error}"),
            Ok(mut copy) => {
                assert_ne!(copy.ttb(), source.ttb());
                for &address in &addresses { assert_eq!(copy.leaf(address), source.leaf(address)); }
                copy.unmap_pages(0x4000..0x8000).unwrap();
                copy.map_pages(0x4000..0x8000, 0xa000000, RW, false).unwrap();
                assert_eq!(source.translate(0x4000).unwrap(), Some(0x9000000));
                source.unmap_pages((1 << 40)..(1 << 40) + PAGE as u64).unwrap();
                assert_eq!(copy.translate(1 << 40).unwrap(), Some(0x9000000 + 3 * PAGE as u64));
                succeeded = true;
            }
        }
        assert_eq!(PAGES.with_borrow(|p| p.len()), page_count, "fork unwind leaked or freed source pages");
        assert_eq!(source.translate(0x4000).unwrap(), Some(0x9000000));
        if succeeded { break; }
    }
    assert!(succeeded);
    println!("PASS async tree forks: independent parents/leaves, 42-bit addresses, permission preservation, allocation-failure unwind and source lifetime");
}
fn main() {
    check_cursors();
    check_forks();
    check_tables();
    check_allocation_errors();
    vm::check();
    assert!(PAGES.with_borrow(|p| p.is_empty()));
}

include!("pipeline.rs");
fn check_cursors() {
    for capacity in [256, 0x500] {
        for target in 0..capacity {
            assert!(reached(target, target, capacity));
            for distance in 1..=64 {
                assert!(reached((target + distance) % capacity, target, capacity));
                assert!(!reached((target + capacity - distance) % capacity, target, capacity));
            }
        }
        assert!(!reached(capacity, 0, capacity));
        assert!(!reached(0, capacity, capacity));
    }
    println!("PASS retirement cursors: every target in both rings, forward/backward windows, wrap and invalid cursors");
}
