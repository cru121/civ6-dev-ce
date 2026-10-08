# SPDX-License-Identifier: AGPL-3.0-only
# Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W. Copyright (C) 2026 cru121. AGPL-3.0 (see LICENSE.txt).
"""Level-2 chunking shared by gen_selftest.py and frida_level2.py: env DEVCE_L2CHUNK=k/n (1-based) selects the k-th of n contiguous slices of the
non-hot entries sorted by (interface, name). Unset = all. The generated Lua window and the Frida stubs MUST use the same value."""
import os


def hot(e):
    n = e.get('nav')
    return bool(n) and n['owner'] in ('Lua::PlayerReference', 'Game')


def chunk_set(man):
    """Set of e['lua'] names in the selected chunk (None = no chunking)."""
    spec = os.environ.get('DEVCE_L2CHUNK')
    if not spec:
        return None
    k, n = (int(x) for x in spec.split('/'))
    elig = sorted((e for e in man if not hot(e)), key=lambda e: (e['iface'], e['lua']))
    size = -(-len(elig) // n)
    return {e['lua'] for e in elig[(k - 1) * size: k * size]}
