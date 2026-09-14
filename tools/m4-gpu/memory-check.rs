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
    static TLB: RefCell<Vec<(Option<u8>,u64,u64)>> = const { RefCell::new(Vec::new()) };
    static PUSHES: Cell<usize> = const { Cell::new(0) };
    static FAIL_PUSH: Cell<Option<usize>> = const { Cell::new(None) };
    static TREE_GETS: Cell<usize> = const { Cell::new(0) };
    static FAIL_NODE: Cell<Option<usize>> = const { Cell::new(None) };
}
// The kernel supplies balanced RB trees. This stand-in preserves their public
// lookup/cursor/ownership semantics and injects node-allocation failures.
struct RBTree<K, V>(BTreeMap<K, V>);
struct RBTreeNode<K, V>(K, V);
struct Cursor<'a, K, V>(&'a BTreeMap<K, V>, &'a K);
impl<K: Ord, V> RBTree<K, V> {
    fn new() -> Self { Self(BTreeMap::new()) }
    fn get(&self, key: &K) -> Option<&V> {
        TREE_GETS.set(TREE_GETS.get() + 1);
        self.0.get(key)
    }
    fn insert(&mut self, node: RBTreeNode<K, V>) -> Option<RBTreeNode<K, V>> {
        let old = self.0.remove_entry(&node.0).map(|(k,v)| RBTreeNode(k,v));
        self.0.insert(node.0, node.1);
        old
    }
    fn try_create_and_insert(&mut self, key: K, value: V, flags: u32) -> Result {
        self.insert(RBTreeNode::new(key, value, flags)?);
        Ok(())
    }
    fn remove(&mut self, key: &K) -> Option<V> { self.0.remove(key) }
    fn values(&self) -> impl Iterator<Item=&V> { self.0.values() }
    fn cursor_lower_bound(&self, key: &K) -> Option<Cursor<'_, K,V>> {
        self.0.range(key..).next().map(|(k,_)| Cursor(&self.0,k))
    }
    fn cursor_back(&self) -> Option<Cursor<'_, K,V>> {
        self.0.last_key_value().map(|(k,_)| Cursor(&self.0,k))
    }
}
impl<K, V> RBTreeNode<K,V> {
    fn new(key: K, value: V, _: u32) -> Result<Self> {
        if let Some(left) = FAIL_NODE.get() {
            if left == 0 { FAIL_NODE.set(None); return Err(ENOMEM); }
            FAIL_NODE.set(Some(left - 1));
        }
        Ok(Self(key,value))
    }
}
impl<K: Ord, V> Cursor<'_, K,V> {
    fn current(&self) -> (&K,&V) { self.0.get_key_value(self.1).unwrap() }
    fn peek_prev(&self) -> Option<(&K,&V)> { self.0.range(..self.1).next_back() }
}
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn new() -> Self {
        Self(Vec::new())
    }
    fn reserve(&mut self, n: usize, _: u32) -> Result { self.0.reserve(n); Ok(()) }
    fn remove(&mut self, index: usize) -> Result<T> { Ok(self.0.remove(index)) }
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
impl<T> IntoIterator for KVec<T> {
    type Item = T;
    type IntoIter = std::vec::IntoIter<T>;
    fn into_iter(self) -> Self::IntoIter { self.0.into_iter() }
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
    pub fn tlbi_range(asid: Option<u8>, start: u64, end: u64) {
        super::TLB.with_borrow_mut(|v| v.push((asid,start,end)));
    }
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
            .values()
            .flat_map(|r| r.pages.iter().flat_map(|p| p.device_bytes()))
            .collect()
    }
    pub fn check() {
        let base = 0x40000;
        let mut space = FirmwareSpace {
            table: UatPageTable::new_with_ias(42, 42).unwrap(),
            regions: RBTree::new(),
            dirty_regions: RefCell::new(KVec::new()),
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
        space.regions.get(&base).unwrap().pages[0].device_write(PAGE - 8, &[0x5a; 5]);
        space.regions.get(&base).unwrap().pages[2].device_write(8, &[0xa5; 7]);
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
        let page = &space.regions.get(&next).unwrap().pages[0];
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
        assert_eq!(space.regions.get(&next).unwrap().pages[0].device_bytes()[41], 0x67);
        let before = image(&space);
        reset();
        assert_eq!(space.write_live(u64::MAX - 3, &[1; 8]), Err(EINVAL));
        assert_eq!(space.zero_live(base + 3 * PAGE as u64 - 2, 8), Err(EINVAL));
        space.write_live(base, &[]).unwrap();
        assert_eq!(image(&space), before);
        assert_eq!(BARRIERS.get(), 0);
        println!("PASS firmware memory: dirty-only cleaning, three-page writes, two-page zeroing, firmware neighbors, initialization, errors, two barriers per batch");

        // Grow history in non-address order. A clean sync must perform no
        // region/table lookups, while one dirty region needs just one lookup.
        let history = 0x1000000;
        for i in 0..128u64 {
            let va = history + ((i * 73) % 128) * 2 * PAGE as u64;
            space.alloc(va, PAGE, pgtable::prot::PROT_FW_PRIV_RW).unwrap();
            space.write(va, &(i as u32).to_le_bytes()).unwrap();
        }
        space.sync();
        reset();
        space.sync();
        assert_eq!((TREE_GETS.get(), CLEAN.get()), (0, 0));
        for i in 0..128u64 {
            let va = history + ((i * 73) % 128) * 2 * PAGE as u64;
            assert_eq!(space.read_u32(va).unwrap(), i as u32);
            assert!(space.region(va + PAGE as u64, 1).is_err());
            assert!(space.region(va + PAGE as u64 - 1, 2).is_err());
        }
        assert!(space.region(base - 1, 1).is_err());
        assert!(space.region(u64::MAX, 1).is_err());
        space.write(history + 2, &[0x11]).unwrap();
        space.write(history + 3, &[0x22]).unwrap();
        assert_eq!(space.dirty_regions.borrow().len(), 1);
        reset();
        space.sync();
        assert_eq!((TREE_GETS.get(), CLEAN.get()), (1, PAGE / 64));

        // Publishing a dirty page live must not duplicate its region entry,
        // and a subsequent unpublished edit must still reach the device.
        space.write(history, &[0x33]).unwrap();
        space.write_live(history + 1, &[0x44]).unwrap();
        space.write(history + 2, &[0x55]).unwrap();
        assert_eq!(space.dirty_regions.borrow().len(), 1);
        space.sync();
        assert_eq!(space.read_u32(history).unwrap(), 0x22554433);

        // Queue-allocation failure must precede the byte write and membership.
        FAIL_PUSH.set(Some(0));
        assert_eq!(space.write(history, &[0xff]), Err(ENOMEM));
        assert!(space.dirty_regions.borrow().is_empty());
        assert_eq!(space.read_u32(history).unwrap(), 0x22554433);
        space.write(history, &[0xaa]).unwrap();
        let keep = history + 2 * PAGE as u64;
        space.write(keep, &[0xbb]).unwrap();
        space.release_region(history).unwrap();
        assert_eq!(&**space.dirty_regions.borrow(), &[keep]);
        assert_eq!(space.read_u32(history), Err(EINVAL));
        space.alloc(history, PAGE, pgtable::prot::PROT_FW_PRIV_RW).unwrap();
        space.alloc(history + PAGE as u64, PAGE, pgtable::prot::PROT_FW_PRIV_RW).unwrap();
        assert_eq!(space.write(history + PAGE as u64 - 1, &[1,2]), Err(EINVAL));
        assert_eq!(space.alloc(history, PAGE, pgtable::prot::PROT_FW_PRIV_RW), Err(EEXIST));
        space.sync();
        assert_eq!(space.read_u32(history).unwrap(), 0);
        assert_eq!(space.read_u32(keep).unwrap() & 0xff, 0xbb);
        reset();
        space.sync();
        assert_eq!((TREE_GETS.get(), CLEAN.get()), (0,0));
        println!("PASS indexed firmware memory: 128 out-of-order regions, bounds/gaps/adjacency, empty-sync zero lookups, one dirty-region lookup, live-write/re-dirty, queue failure, pending removal and VA reuse");
    }

    pub fn check_node_errors() {
        let baseline = PAGES.with_borrow(|p|p.len());
        for fail_after in 0..3 {
            let mut space = FirmwareSpace {
                table: UatPageTable::new_with_ias(42,42).unwrap(),
                regions: RBTree::new(), dirty_regions: RefCell::new(KVec::new()),
                external: KVec::new(),
            };
            space.sync();
            FAIL_NODE.set(Some(fail_after));
            assert_eq!(space.alloc(0x40000,PAGE,pgtable::prot::PROT_FW_PRIV_RW),Err(ENOMEM));
            assert_eq!(space.table.translate(0x40000).unwrap(),None);
            assert!(space.regions.get(&0x40000).is_none());
            assert!(space.dirty_regions.borrow().is_empty());
            space.sync();
            space.alloc(0x40000,PAGE,pgtable::prot::PROT_FW_PRIV_RW).unwrap();
            space.sync();
            assert_eq!(space.read_u32(0x40000).unwrap(),0);
            drop(space);
            assert_eq!(PAGES.with_borrow(|p|p.len()),baseline);
        }
        println!("PASS firmware index allocation failures: region and both child-table nodes, mapping/dirty-queue unwind, retry and complete teardown");
    }
}
fn reset() {
    CLEAN.set(0);
    INVALIDATE.set(0);
    BARRIERS.set(0);
    TREE_GETS.set(0);
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

#[path="compute.rs"] mod g16_compute;
fn check_compute_storage() {
    let baseline = PAGES.with_borrow(|p| p.len());
    let mut vm = vm::AddressSpace::new().unwrap();
    let roots = vm.roots();
    let base = 0x6800000000u64;
    vm.init_compute_private(base..base + 37 * 0x24000);
    // Original shader resources, including their permissions, stay untouched.
    vm.low.map_pages(0x10000058000..0x1000005c000, 0x9000000,
        pgtable::prot::PROT_GPU_SHARED_RO, false).unwrap();
    let original = vm.low.leaf(0x10000058000).unwrap();
    let mut p = g16_compute::Parameters {cdm:0x1400000000, cdm_end:0x1400000040,
        sampler:0,sampler_count:0,scratch:0,marker:0,save_area:0};
    let mut physical = std::collections::BTreeSet::new();
    let mut leases = Vec::new();
    for _ in 0..36 {
        leases.push(vm.prepare_compute(&mut p).unwrap());
        assert_eq!(vm.roots().low, roots.low);
        assert_eq!(vm.roots().high, roots.high);
        assert_eq!(p.marker, p.scratch + 0x20000);
        for va in (p.scratch..p.marker + 0x4000).step_by(PAGE) {
            let pa = vm.low.translate(va).unwrap().unwrap();
            assert!(physical.insert(pa), "live Works must not share scratch backing");
            let page = unsafe { Page::borrow_phys_unchecked(&pa) };
            page.with_pointer_into_page(0,PAGE,|ptr| {
                assert!(unsafe { std::slice::from_raw_parts(ptr,PAGE) }.iter().all(|b|*b==0));
                Ok(())
            }).unwrap();
        }
    }
    assert_eq!(vm.low.leaf(0x10000058000).unwrap(), original);
    for address in [g16_compute::SCRATCH, g16_compute::MARKER] {
        assert_eq!(vm.low.translate(address).unwrap(),None);
    }
    FAIL_NODE.set(Some(0));
    assert_eq!(vm.prepare_compute(&mut p),Err(ENOMEM));
    FAIL_NODE.set(None);
    assert_eq!(vm.prepare_compute(&mut p),Err(ENOMEM), "failed reservation is not reused");
    assert_eq!(vm.low.leaf(0x10000058000).unwrap(), original);
    let retained = PAGES.with_borrow(|p| p.len());
    for round in 0..10 {
        // Release out of order to exercise adjacency/coalescing; every active
        // lease remains distinct until its explicit retirement.
        while !leases.is_empty() {
            let index = leases.len() / 2;
            vm.release_scratch(leases.remove(index)).unwrap();
        }
        for index in 0..36 {
            leases.push(vm.prepare_compute(&mut p).unwrap());
            assert_eq!(p.scratch, base + index * 0x24000);
            let pa = vm.low.translate(p.marker).unwrap().unwrap();
            let page = unsafe { Page::borrow_phys_unchecked(&pa) };
            page.with_pointer_into_page(0, PAGE, |ptr| {
                assert!(unsafe { std::slice::from_raw_parts(ptr,PAGE) }.iter().all(|b|*b==0));
                unsafe { std::ptr::write_bytes(ptr, 0xa5, PAGE) };
                Ok(())
            }).unwrap();
        }
        assert_eq!(PAGES.with_borrow(|p|p.len()), retained, "reuse round {round}");
    }
    drop(vm);
    assert_eq!(PAGES.with_borrow(|p|p.len()),baseline,"VM destruction releases all private backing");
    println!("PASS compute storage: 36 commands share roots, distinct zeroed backing, caller RO PTE unchanged, no fixed scratch aliases, exhaustion/failure retention and VM teardown");
}

fn main() {
    check_client_index();
    check_dirty_table_history();
    check_compute_storage();
    check_invalidations();
    check_cursors();
    check_tables();
    check_allocation_errors();
    vm::check_node_errors();
    vm::check();
    assert!(PAGES.with_borrow(|p| p.is_empty()));
}

fn check_dirty_table_history() {
    use pgtable::{prot::PROT_GPU_SHARED_RW as RW, UatPageTable};
    let mut table = UatPageTable::new_with_ias(42,42).unwrap();
    for i in 0..64u64 {
        let va = (i + 1) << 25;
        table.map_pages(va..va + PAGE as u64, 0x9000000, RW, false).unwrap();
    }
    table.sync();
    reset();
    table.sync();
    assert_eq!((TREE_GETS.get(), CLEAN.get()), (0,0));
    let va = 1 << 25;
    table.unmap_pages(va..va + PAGE as u64).unwrap();
    table.map_pages(va..va + PAGE as u64, 0x9100000, RW, false).unwrap();
    reset();
    table.sync();
    assert_eq!((TREE_GETS.get(), CLEAN.get()), (1,PAGE / 64));
    // First push records the invalidation, second queues the clean. Failing
    // the latter must leave the PTE unchanged and allow the next edit to retry.
    table.clear_invalidations();
    FAIL_PUSH.set(Some(1));
    assert_eq!(table.unmap_pages(va..va + PAGE as u64), Err(ENOMEM));
    assert_eq!(table.translate(va).unwrap(),Some(0x9100000));
    reset(); table.sync();
    assert_eq!((TREE_GETS.get(), CLEAN.get()),(0,0));
    table.unmap_pages(va..va + PAGE as u64).unwrap();
    reset(); table.sync();
    assert_eq!((TREE_GETS.get(), CLEAN.get()),(1,PAGE / 64));
    println!("PASS dirty-table history: 64 leaves, clean sync zero lookups, repeated edits clean once, failed enqueue leaves PTE unchanged and retry publishes");
}

fn check_client_index() {
    let baseline = PAGES.with_borrow(|p|p.len());
    let base = 0x4000000u64;
    let mut space = vm::AddressSpace::new().unwrap();
    assert_eq!(space.alloc_tvb_blocks(base, 2),Ok(true));
    for block in 0..2u64 {
        let start = base + block * 0x28000;
        for offset in (0..0x20000).step_by(PAGE) {
            let va = start + offset as u64;
            space.write_low(va + 17, &[0x9a]).unwrap();
            let phys = space.low.translate(va).unwrap().unwrap();
            assert_eq!(unsafe {Page::borrow_phys_unchecked(&phys)}.device_bytes()[17],0x9a);
        }
        assert_eq!(space.low.translate(start + 0x20000).unwrap(),None);
        assert_eq!(space.write_low(start + 0x20000, &[1]),Err(EINVAL));
    }
    assert_eq!(space.alloc_tvb_blocks(base,1),Err(EEXIST));
    drop(space);
    assert_eq!(PAGES.with_borrow(|p|p.len()),baseline);
    for fail_after in 0..16 {
        let mut space = vm::AddressSpace::new().unwrap();
        let roots_only = PAGES.with_borrow(|p|p.len());
        FAIL_NODE.set(Some(fail_after));
        assert_eq!(space.alloc_tvb_blocks(base,2),Ok(false));
        assert_eq!(space.low.translate(base).unwrap(),None);
        assert_eq!(PAGES.with_borrow(|p|p.len()),roots_only);
        assert_eq!(space.alloc_tvb_blocks(base,2),Ok(true));
    }
    assert_eq!(PAGES.with_borrow(|p|p.len()),baseline);
    println!("PASS client page index: TVB writes, unmapped guards, duplicate refusal, all 16 node allocation failures unwind before mapping and permit retry");
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

fn check_invalidations() {
    use pgtable::{UatPageTable,prot::PROT_GPU_SHARED_RW as RW};
    let mut table = UatPageTable::new_with_ias(42,42).unwrap();
    table.map_pages(0x4000..0x8000,0x9000000,RW,false).unwrap();
    table.map_pages(0x8000..0xc000,0x9004000,RW,false).unwrap();
    table.map_pages(0x100000..0x104000,0x9008000,RW,false).unwrap();
    table.sync(); TLB.with_borrow_mut(|v| v.clear());
    table.invalidate(Some(9));
    assert_eq!(TLB.with_borrow(|v| v.clone()),
        vec![(Some(9),0x4000,0xc000),(Some(9),0x100000,0x104000)]);
    table.clear_invalidations(); TLB.with_borrow_mut(|v| v.clear());
    table.invalidate(None);
    assert!(TLB.with_borrow(|v| v.is_empty()));
    table.unmap_pages(0x8000..0xc000).unwrap(); table.sync(); table.invalidate(None);
    assert_eq!(TLB.with_borrow(|v| v.clone()),vec![(None,0x8000,0xc000)]);
    println!("PASS targeted TLB: adjacent edits merge, disjoint mappings stay separate, ASID/global VA scope, clean sync does not invalidate");
}
