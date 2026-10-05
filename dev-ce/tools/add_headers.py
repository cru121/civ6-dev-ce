#!/usr/bin/env python3
"""Prepend the licence header to the files that are new in Dev CE (not to files that come from upstream unchanged). Idempotent: skips files that already carry the SPDX line.
Usage: python add_headers.py ROOT   (ROOT = a tree that contains src/ and tools/ or the fork checkout; files are looked up by name)"""
import os, sys

SPDX = 'SPDX-License-Identifier: AGPL-3.0-only'
LINES = [SPDX,
         'Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).',
         'Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.']

NEW_FILES = {   # file name -> comment style
    'DevBridge.cpp': '//', 'DevBridge.h': '//', 'DevNativeTable.cpp': '//', 'GameProcessor.cpp': '//', 'GameProcessor.h': '//',
    'frida_level2.py': '#', 'gen_bridge.py': '#', 'gen_selftest.py': '#', 'hang_dump.py': '#', 'level3_report.py': '#', 'merge_status.py': '#',
    'owner_analysis.py': '#', 'pdb_sym.py': '#', 'prep_batch.py': '#', 'add_headers.py': '#', 'civ_watchdog.ps1': '#', 'devce.js': '//', 'CE_Proc.lua': '--',
}

root = sys.argv[1]
done = 0
for dp, dn, fn in os.walk(root):
    if any(x in dp for x in ('.git', 'asmjit', 'build')):
        continue
    for f in fn:
        style = NEW_FILES.get(f)
        if not style:
            continue
        p = os.path.join(dp, f)
        raw = open(p, 'rb').read()
        text = raw.decode('utf-8-sig')
        if SPDX in text[:600]:
            continue
        crlf = '\r\n' in text
        t = text.replace('\r\n', '\n')
        head = '\n'.join('%s %s' % (style, l) for l in LINES) + '\n'
        if t.startswith('#!'):                       # keep the shebang first
            first, rest = t.split('\n', 1)
            t = first + '\n' + head + rest
        else:
            t = head + t
        if crlf:
            t = t.replace('\n', '\r\n')
        open(p, 'wb').write(t.encode('utf-8'))
        done += 1
        print('header added:', os.path.relpath(p, root))
print(done, 'files changed')
