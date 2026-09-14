#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright The Gravity Linux Contributors
"""Execute actual admission, scene-lease and UAPI barrier methods on the host."""
from pathlib import Path
import runpy
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
block = runpy.run_path(str(Path(__file__).with_name("check-memory.py")))["block"]
src = root / "drivers/gpu/drm/asahi"
code = """type Result<T = ()> = std::result::Result<T, i32>;
const EBUSY:i32=16; const EIO:i32=5; type KVec<T> = Vec<T>;
mod g16 { #[derive(Clone,Copy,Debug,PartialEq)] pub struct Stamp { pub address:u64, pub value:u32, pub event:u32 } }
struct Tvb { scenes:u64 }
"""
code += block((src / "g16_runtime.rs").read_text(), "fn ring_room(")
code += block((src / "g16_drm.rs").read_text(), "fn resolve_barriers(")
code += "impl Tvb {" + "".join(block((src / "g16_tvb.rs").read_text(), "pub(crate) fn " + n + "(")
                                for n in ("reserve_scene", "release_scene")) + "}"
code += r'''
fn main() {
    for capacity in [256u32, 1280] {
        for tail in 0..capacity {
            for entries in 1..=4 {
                for distance in [0, 1, capacity/2-entries-1, capacity/2-entries, capacity-1] {
                    let cursor=(tail+capacity-distance)%capacity;
                    let expected=distance+entries<capacity/2;
                    assert_eq!(ring_room(tail,cursor,tail,entries,capacity),expected);
                    assert_eq!(ring_room(tail,tail,cursor,entries,capacity),expected);
                }
                assert!(!ring_room(tail,capacity,tail,entries,capacity));
                assert!(!ring_room(tail,tail,capacity,entries,capacity));
            }
        }
    }
    println!("SCHEDULING_PASS whole-publication ring credits, both consumers, wrap, half-ring and invalid cursors");
    let mut pool=Tvb{scenes:0};
    for i in 0..36 { assert_eq!(pool.reserve_scene(),Ok(i)); }
    assert_eq!(pool.reserve_scene(),Err(EBUSY));
    for i in [17,0,35,9] {
        pool.release_scene(i); assert_eq!(pool.reserve_scene(),Ok(i));
        assert_eq!(pool.reserve_scene(),Err(EBUSY));
    }
    println!("SCHEDULING_PASS 36 distinct scene leases, saturation and out-of-order retirement reuse");
    let stamp=|v| Some(g16::Stamp{address:v,value:v as u32,event:v as u32});
    let history=[vec![stamp(10),stamp(11),stamp(12)],vec![stamp(20),stamp(21)]];
    assert_eq!(resolve_barriers(&history,[0xffff,0xffff]),Ok([None,None]));
    assert_eq!(resolve_barriers(&history,[0,0]),Ok([stamp(10),stamp(20)]));
    assert_eq!(resolve_barriers(&history,[2,1]),Ok([stamp(12),stamp(21)]));
    assert_eq!(resolve_barriers(&history,[1,0xffff]),Ok([stamp(11),None]));
    assert_eq!(resolve_barriers(&history,[3,0]),Err(EIO));
    assert_eq!(resolve_barriers(&[vec![None],vec![None]],[0,0]),Ok([None,None]));
    println!("SCHEDULING_PASS UAPI NONE, previous-submit zero, independent engine indices and unavailable predecessor");
}
'''
with tempfile.TemporaryDirectory(prefix="m4-scheduling-") as tmp:
    path=Path(tmp)
    (path / "test.rs").write_text(code)
    subprocess.run(["rustc", "--edition=2021", str(path / "test.rs"), "-o", str(path / "test")],check=True)
    subprocess.run([str(path / "test")],check=True,timeout=15)
