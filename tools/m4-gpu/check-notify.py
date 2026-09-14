#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright The Gravity Linux Contributors
"""Exercise the actual driver wakeup methods with host mutex/condvar stand-ins.

This checks generation races and watchdog/crash behavior. Hardware qualification
must separately check RTKit delivery and firmware completion ordering.
"""
from pathlib import Path
import subprocess
import tempfile
import runpy


def main():
    root = Path(__file__).resolve().parents[2]
    block = runpy.run_path(str(Path(__file__).with_name("check-memory.py")))["block"]
    source = (root / "drivers/gpu/drm/asahi/g16_notify.rs").read_text()
    methods = "\n".join(block(source, f"pub(crate) fn {name}(") for name in ("notify", "snapshot", "wait"))
    code = Path(__file__).with_name("notify-check.rs").read_text()
    code += "\n" + block(source, "struct State {")
    code += "\nimpl Notifications {\n" + methods + "\n}\n"
    with tempfile.TemporaryDirectory(prefix="m4-notify-") as tmp:
        out = Path(tmp)
        (out / "test.rs").write_text(code)
        subprocess.run(["rustc", "--edition=2021", "-Awarnings", str(out / "test.rs"), "-o", str(out / "test")], check=True)
        subprocess.run([str(out / "test")], check=True, timeout=15)


if __name__ == "__main__":
    main()
