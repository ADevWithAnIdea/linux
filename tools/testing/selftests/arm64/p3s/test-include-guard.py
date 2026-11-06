#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host preprocessing checks for P3S opt-in source/header boundaries."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root = next(p for p in Path(__file__).resolve().parents
            if (p / 'include/linux/p3s_user_pages.h').is_file())
header = (root / 'include/linux/p3s_user_pages.h').read_bytes()
with tempfile.TemporaryDirectory(prefix='p3s-include-guard-') as temp:
    temp = Path(temp)
    (temp / 'linux').mkdir()
    (temp / 'linux/p3s_user_pages.h').write_bytes(header)
    (temp / 'linux/p3s.h').write_text('')
    (temp / 'wrapper.h').write_text('#include <linux/p3s_user_pages.h>\n')
    cases = [
        ('direct', '#include <linux/p3s_user_pages.h>\n', None, True),
        ('header', '#include "wrapper.h"\n', None, False),
        ('included-c', '#define P3S_USER_PAGES_C_WRAPPER\n#include "included.c"\n',
         '#include <linux/p3s_user_pages.h>\n', True),
        ('nested-header', '#define P3S_USER_PAGES_C_WRAPPER\n#include "included.c"\n',
         '#include "wrapper.h"\n', False),
    ]
    for name, source, included, accepted in cases:
        (temp / 'test.c').write_text(source)
        if included is not None:
            (temp / 'included.c').write_text(included)
        result = subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                                ['-E', '-I', str(temp), str(temp / 'test.c')],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        assert (result.returncode == 0) == accepted, (name, result.stderr.decode())
        if not accepted:
            assert b'Must not include' in result.stderr
        print('PASS:', name)
