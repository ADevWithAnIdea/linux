#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright The Gravity Linux Contributors
"""Run actual M4 memory methods with host page/cache stand-ins.

Checks bookkeeping, byte preservation, and maintenance counts; hardware tests
are still required for real cache/TLB ordering. No Python shim is involved.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    for end in range(opening + 1, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(marker)


def cache_ops(source):
    source = re.sub(r'core::arch::asm!\("dc cvac, \{addr\}", addr = in\(reg\) p.add\(off\)\)',
                    'crate::cache::clean(p.add(off))', source)
    source = re.sub(r'core::arch::asm!\("dc ivac, \{addr\}", addr = in\(reg\) p.add\(off\)\)',
                    'crate::cache::invalidate(p.add(off))', source)
    assert 'core::arch::asm!' not in source
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    src = args.kernel / 'drivers/gpu/drm/asahi'
    tables = (src / 'm4_pgtable.rs').read_text()
    tables = 'use super::*; use crate::util::align; use core::sync::atomic::{AtomicU64, Ordering};\n' + tables[tables.index('/// Number of bits in a page offset.'):]
    vm = (src / 'g16_vm.rs').read_text()
    impl = vm[vm.index('impl FirmwareSpace {'):]
    vm_code = ['use super::*; use pgtable::{UatPageTable, UAT_PGSZ};',
               block(vm, 'struct FirmwareRegion {'), block(vm, 'pub(crate) struct FirmwareSpace {'),
               block(vm, 'fn clean_page('), 'impl FirmwareSpace {']
    for name in ('alloc', 'write', 'init_page', 'write_live', 'zero_live', 'update_live', 'sync'):
        vm_code.append(block(impl, ('fn ' if name == 'update_live' else 'pub(crate) fn ') + name + '('))
    vm_code.append('}')
    with tempfile.TemporaryDirectory(prefix='m4-memory-') as tmp:
        out = Path(tmp)
        (out / 'pgtable.rs').write_text(cache_ops(tables))
        (out / 'vm.rs').write_text(cache_ops('\n'.join(vm_code)))
        runtime = (src / 'g16_runtime.rs').read_text()
        (out / 'pipeline.rs').write_text(block(runtime, 'fn reached('))
        (out / 'test.rs').write_text(Path(__file__).with_name('memory-check.rs').read_text())
        subprocess.run(['rustc', '--edition=2021', '--cfg', 'test', '-Awarnings', str(out / 'test.rs'), '-o', str(out / 'test')], check=True)
        subprocess.run([str(out / 'test')], check=True)


if __name__ == '__main__':
    main()
