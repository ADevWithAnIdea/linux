#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright The Gravity Linux Contributors
"""Execute actual transport staging/publication with recorded MMIO and mailbox IO."""
from pathlib import Path
import runpy
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
block = runpy.run_path(str(Path(__file__).with_name("check-memory.py")))["block"]
runtime = (root / "drivers/gpu/drm/asahi/g16_runtime.rs").read_text()
code = r'''
use std::{cell::RefCell, collections::BTreeMap, ops::{Deref,DerefMut}};
type Result<T=()> = std::result::Result<T,i32>;
const GFP_KERNEL:u32=0; const EIO:i32=5; const ENODEV:i32=19; const ENOSPC:i32=28;
macro_rules! dev_info { ($($t:tt)*) => {}; }
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn push(&mut self,x:T,_:u32)->Result {self.0.push(x);Ok(())}
    fn reserve(&mut self,n:usize,_:u32)->Result {self.0.reserve(n);Ok(())}
}
impl<T> Deref for KVec<T> {type Target=Vec<T>;fn deref(&self)->&Vec<T>{&self.0}}
impl<T> DerefMut for KVec<T> {fn deref_mut(&mut self)->&mut Vec<T>{&mut self.0}}
mod mem {pub fn sync() {}}
mod pgtable {pub mod prot {pub const PROT_FW_PRIV_RW:u8=1;pub const PROT_FW_SHARED_RW:u8=2;}}
mod module_parameters {
    pub struct Param;
    impl Param {pub fn value(&self)->&u64{&0}}
    #[allow(non_upper_case_globals)] pub static fw_trace:Param=Param;
}
mod g16_fw {
    pub const BUNDLE_ADDRESS:u64=0;
    pub struct Channel {pub state:[u64;3],pub ring:u64}
    pub struct MainConfig {pub channels:[Channel;12]}
    impl MainConfig {pub fn bootstrap(_:u64)->Self {
        Self {channels:std::array::from_fn(|i|Channel {
            state:[0,0,0xffff_fc20_0002_0000+i as u64*32],ring:0x80000+i as u64*0x4000})}
    }}
    // Wire bytes are checked separately. This stand-in records all message
    // arguments so the test can check routing, first-use and final-head state.
    pub mod queue {pub fn channel(q:u64,h:u16,e:u8,s:u32,n:bool,v:u64)->[u8;24] {
        let mut b=[0;24];b[..8].copy_from_slice(&q.to_le_bytes());
        b[8..10].copy_from_slice(&h.to_le_bytes());b[10]=e;b[11]=s as u8;
        b[12]=n as u8;b[16..24].copy_from_slice(&v.to_le_bytes());b
    }}
}
struct Io {tails:RefCell<[u32;12]>}
impl Io {
    fn try_access(&self)->Option<&Self>{Some(self)}
    fn try_read32(&self,o:usize)->Result<u32>{Ok(self.tails.borrow()[(o-0xd60000)/32])}
    fn try_write32(&self,v:u32,o:usize)->Result{self.tails.borrow_mut()[(o-0xd60000)/32]=v;Ok(())}
}
#[derive(Default)] struct Firmware {writes:BTreeMap<u64,Vec<u8>>,
    allocations:Vec<(u64,usize,u8)>,syncs:usize,fail_alloc:Option<usize>}
impl Firmware {
    fn alloc(&mut self,a:u64,n:usize,p:u8)->Result {
        if self.fail_alloc==Some(self.allocations.len()){return Err(ENOSPC)}
        assert!(!self.allocations.iter().any(|&(old,len,_)|a<old+len as u64 && old<a+n as u64));
        self.allocations.push((a,n,p));Ok(())
    }
    fn sync(&mut self) {self.syncs+=1;}
    fn write_live(&mut self,a:u64,b:&[u8])->Result{self.writes.insert(a,b.to_vec());Ok(())}}
#[derive(Default)] struct RtKit {messages:Vec<(u8,u64)>,fail:bool}
impl RtKit {
    fn as_mut(&mut self)->&mut Self {self}
    fn send_message(&mut self,e:u8,m:u64)->Result {
        if self.fail{return Err(EIO)} self.messages.push((e,m));Ok(())
    }
}
struct Bootstrap {
    publications:KVec<Publication>,_sgx:Io,_firmware_space:Option<Firmware>,
    _rtkit:RtKit,render_failed:bool,batch_works:usize,
    next_work_va:u64,work_arenas:[std::ops::Range<u64>;2],
    free_work:[KVec<u64>;2],flights:Vec<()>,
}
'''
code += "#[derive(Clone,Copy)]\n" + block(runtime, "struct Lane {")
code += "#[derive(Clone,Copy)]\n" + block(runtime, "pub(super) struct Publication {").replace("pub(super) ", "")
code += "impl Bootstrap {" + block(runtime, "fn allocate_work(") + block(runtime, "fn stage_queue(") + block(runtime, "pub(crate) fn flush(") + "}"
code += r'''
fn backend()->Bootstrap {Bootstrap {
    publications:KVec(Vec::new()),_sgx:Io{tails:RefCell::new([0;12])},
    _firmware_space:Some(Firmware::default()),_rtkit:RtKit::default(),
    render_failed:false,batch_works:0,next_work_va:0x10000000,work_arenas:[0..0,0..0],
    free_work:[KVec(Vec::new()),KVec(Vec::new())],flights:Vec::new(),
}}
fn lane(queue:u64,head:u32,new:bool)->Lane {Lane{queue,pointers:queue+0x4000,ring:queue+0x8000,head,new}}
fn main() {
    let mut pool=backend();let mut slots=Vec::new();
    for i in 0..16 {for render in [false,true] {
        let va=pool.allocate_work(render).unwrap();
        let extent=if render {0x1c000}else{0x8000};
        assert!(!slots.iter().any(|&(old,len)|va<old+len && old<va+extent));
        slots.push((va,extent));
        let fw=pool._firmware_space.as_ref().unwrap();
        assert_eq!(fw.syncs,if i==0 && !render {1}else{2});
        assert!(fw.allocations.contains(&(va,if render {0x10000}else{0x4000},1)));
        if render {assert!(fw.allocations.contains(&(va+0x10000,0xc000,2)));}
        else {assert!(fw.allocations.contains(&(va+0x4000,0x4000,2)));}
    }}
    let fw=pool._firmware_space.as_ref().unwrap();assert_eq!(fw.allocations.len(),64);
    for round in 0..100 {
        for &(va,extent) in &slots {
            pool.free_work[usize::from(extent==0x1c000)].push(va,GFP_KERNEL).unwrap();
        }
        for _ in 0..16 {for render in [false,true] {
            let va=pool.allocate_work(render).unwrap();
            assert!(slots.contains(&(va,if render {0x1c000}else{0x8000})));
        }}
        assert_eq!(pool._firmware_space.as_ref().unwrap().allocations.len(),64,"round {round}");
    }
    let before=pool.next_work_va;
    pool._firmware_space.as_mut().unwrap().fail_alloc=Some(67);
    assert_eq!(pool.allocate_work(false),Err(ENOSPC));assert!(pool.work_arenas[0].is_empty());
    assert!(pool.next_work_va>before);
    pool._firmware_space.as_mut().unwrap().fail_alloc=None;
    assert!(pool.allocate_work(false).unwrap()>=before+16*0x8000);
    println!("BATCHING_PASS Work arenas amortize 32 commands over two mapped chunks; permissions, distinct retained slots and failed-refill isolation");
    let mut b=backend();
    for i in 0..32 {assert_eq!(b.stage_queue(5,lane(0x10000,i+1,i==0),32,i as u64+1),Ok(0));}
    assert_eq!(b.publications.len(),1);assert!(b._rtkit.messages.is_empty());
    assert_eq!(b._sgx.tails.borrow()[5],0);
    b.flush().unwrap();
    assert_eq!(b._sgx.tails.borrow()[5],1);assert_eq!(b._rtkit.messages,vec![(0x21,0x0083_0000_0000_0006)]);
    let body=&b._firmware_space.as_ref().unwrap().writes[&(0x80000+5*0x4000)];
    assert_eq!(&body[8..13],&[32,0,32,2,1]);
    assert_eq!(u64::from_le_bytes(body[16..24].try_into().unwrap()),32);
    b.flush().unwrap();assert_eq!(b._rtkit.messages.len(),1);
    println!("BATCHING_PASS 32 Works share one message/kick, final head/sequence, retained first-use and empty flush");

    let mut b=backend();
    for i in 0..32 {for priority in 1..=2 {for stage in 0..3 {
        let ch=priority*3+stage;
        b.stage_queue(ch,lane(0x100000+ch as u64*0x10000,i+1,i==0),32+ch as u32,i as u64).unwrap();
    }}}
    assert_eq!(b.publications.len(),6);b.flush().unwrap();
    assert_eq!(b._rtkit.messages.len(),4);
    for ch in 3..9 {assert_eq!(b._sgx.tails.borrow()[ch],1);}
    println!("BATCHING_PASS independent VDM/FRAG/CDM queues, priorities and deduplicated engine kicks");

    let mut b=backend();b._sgx.tails.borrow_mut()[5]=255;
    assert_eq!(b.stage_queue(5,lane(0x10000,10,true),32,1),Ok(255));
    assert_eq!(b.stage_queue(5,lane(0x30000,20,true),35,2),Ok(0));
    assert_eq!(b.stage_queue(5,lane(0x10000,11,false),32,3),Ok(255));
    b.flush().unwrap();assert_eq!(b._sgx.tails.borrow()[5],1);
    let fw=b._firmware_space.as_ref().unwrap();
    assert_eq!(fw.writes[&(0x80000+5*0x4000+255*24)][8],11);
    assert_eq!(fw.writes[&(0x80000+5*0x4000)][8],20);
    println!("BATCHING_PASS interleaved queues preserve epochs across 255->0 wrap and final producer advances past both");

    b.stage_queue(5,lane(0x10000,12,false),32,4).unwrap();b._rtkit.fail=true;
    assert_eq!(b.flush(),Err(EIO));assert!(b.render_failed);assert!(!b.publications.is_empty());
    println!("BATCHING_PASS publication failure quarantines staged ownership");
}
'''
with tempfile.TemporaryDirectory(prefix="m4-batching-") as tmp:
    path = Path(tmp)
    (path / "test.rs").write_text(code)
    subprocess.run(["rustc", "--edition=2021", "-Awarnings", str(path / "test.rs"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=15)
