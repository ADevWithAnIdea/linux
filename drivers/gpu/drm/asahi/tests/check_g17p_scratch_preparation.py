#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the production scratch-preparation gate for both render modes.

Work stands in for the allocator and records whether preparation was requested.
This checks admission to preparation, not GPU execution or page-table contents.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] / "g17p_boot.rs")
args = parser.parse_args()
source = args.source.read_text()
start = source.index("    pub(crate) fn render_scratch_seed(")
body = source.index("{", start)
end, depth = body + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
method = source[start:end].replace("super::g17p_render::Parameters", "Parameters")
method = method.replace("render::ScratchSeed", "ScratchSeed")
code = r'''
#![allow(dead_code)]
type Result<T> = std::result::Result<T, ()>;
#[derive(Clone, Copy, PartialEq)] enum Phase { Prepared, Running, Failed }
struct Parameters { needs_scratch: bool }
#[derive(Debug)] struct ScratchSeed;
struct Work;
impl Work {
    fn scratch_seed(&self, p: &Parameters) -> Result<Option<ScratchSeed>> {
        Ok(p.needs_scratch.then_some(ScratchSeed))
    }
}
struct Session {
    independent: bool,
    phase: Phase,
    render: Option<Work>,
    dormant_render: Option<Work>,
}
impl Session {
    fn independent_render_roots(&self) -> bool { self.independent }
''' + method + r'''
}
fn main() {
    let large = Parameters { needs_scratch: true };
    let cached = Parameters { needs_scratch: false };
    for independent in [false, true] {
        for dormant in [false, true] {
            let mut session = Session {
                independent, phase: Phase::Running,
                render: (!dormant).then_some(Work),
                dormant_render: dormant.then_some(Work),
            };
            assert!(session.render_scratch_seed(&large).unwrap().is_some(),
                "new geometry was denied preparation: independent={independent}, dormant={dormant}");
            assert!(session.render_scratch_seed(&cached).unwrap().is_none());
            for phase in [Phase::Prepared, Phase::Failed] {
                session.phase = phase;
                assert!(session.render_scratch_seed(&large).unwrap().is_none());
            }
        }
    }
    let empty = Session { independent: false, phase: Phase::Running, render: None, dormant_render: None };
    assert!(empty.render_scratch_seed(&large).unwrap().is_none());
    println!("PASS production scratch gate: single/independent roots, active/dormant owners, cached geometry and lifecycle");
}
'''
with tempfile.TemporaryDirectory(prefix="g17p-scratch-") as tmp:
    root = Path(tmp)
    (root / "main.rs").write_text(code)
    subprocess.run(["rustc", "--edition=2021", str(root / "main.rs"), "-o", str(root / "gate")], check=True)
    subprocess.run([str(root / "gate")], check=True)
