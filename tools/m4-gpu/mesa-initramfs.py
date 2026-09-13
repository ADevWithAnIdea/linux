#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright The Gravity Linux Contributors
"""Expand an optional Mesa runtime overlay and emit gen_init_cpio entries."""
from pathlib import Path, PurePosixPath
import sys
import tarfile

archive, destination = Path(sys.argv[1]), Path(sys.argv[2])
destination.mkdir(parents=True, exist_ok=True)
with tarfile.open(archive, 'r:gz') as source:
    for member in source:
        name = PurePosixPath(member.name)
        if name.is_absolute() or '..' in name.parts or str(name) == '.':
            raise ValueError(f'invalid archive name {member.name!r}')
        target = destination / str(name)
        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
            print(f'dir /{name} {member.mode:04o} 0 0')
        elif member.isfile():
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as data, target.open('wb') as out:
                while block := data.read(1024 * 1024):
                    out.write(block)
            print(f'file /{name} {target} {member.mode:04o} 0 0')
        else:
            raise ValueError(f'unsupported archive type for {name}')
