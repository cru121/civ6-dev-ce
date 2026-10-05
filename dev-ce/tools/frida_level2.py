#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
# Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
"""Level-2 plumbing test of the Dev CE native bridge (Frida live channel + the gameplay self-test script).

  python dev-ce/tools/frida_level2.py arm       # game running with the Dev CE test build, frida/live/live_daemon.py attached
  ... end ONE turn in the game (the test window runs at the start of your next turn) ...
  python dev-ce/tools/frida_level2.py collect   # compare what the native functions received with what the Lua script sent

arm     replaces each of the exposed native functions by a recording stub (frida/live/cmds/devce.js), hooks the DLL's marker function and arms the DLL.
        Until the window opens the stubs only forward to the original functions: the game behaves normally.
window  at the start of the human player's next turn the gameplay script calls every generated method once with sentinel arguments inside a window
        (markers 1 and 2). It only does so after a handshake from this tool (stubs confirmed active), so no real engine code runs with sentinel values.
collect checks, per method: the native function was entered exactly once, `this` non-null, every argument register equals the sentinel (ints, bools as 1),
        and the Lua call got the stub's sentinel return value back. Writes dev-ce/data/level2_results.json.
"""
import json, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'frida', 'live'))
import civ  # noqa: E402

LOG = os.path.expandvars(r"%LOCALAPPDATA%\Firaxis Games\Sid Meier's Civilization VI\Logs\Lua.log")
RET_SENTINEL = {'VOID': 0, 'BOOL': 1, 'INT': 4242, 'UINT': 4243, 'I64': 4244, 'FIXED': 4864}   # FIXED: raw 4864 = 19.0
RET_WANT = {'VOID': 'nil', 'BOOL': 'true', 'INT': '4242', 'UINT': '4243', 'I64': '4244', 'FIXED': '19'}


def call(line, mode='auto'):
    r = civ.ask({'op': 'line', 'line': line, 'mode': mode})
    if not r.get('ok'):
        raise RuntimeError(r.get('error'))
    res = r.get('result')
    if isinstance(res, str) and res.startswith('PENDING'):
        raise RuntimeError(res)
    return res


def manifest():
    return json.load(open(os.path.join(ROOT, 'dev-ce', 'data', 'exposed.json'), encoding='utf-8'))


def hot(e):
    """Functions reached through the Player root or the Game/Map singletons may be called by the engine all the time from several threads (GameStateLock, SetAlive,
    ...). A Frida stub, even one that only forwards, runs JavaScript on every call from every thread: that hung the whole machine twice. They are NEVER stubbed."""
    n = e.get('nav')
    return bool(n) and n['owner'] in ('Lua::PlayerReference', 'Game')


def sentinels(e):
    out = []
    for i, a in enumerate(e['args']):
        out.append({'INT': 11 + i, 'UINT': 21 + i, 'I64': 31 + i, 'BOOL': 1, 'FIXED': (2 + i) * 256 + 128}[a['kind']])   # FIXED: Lua (2+i).5 -> raw
    return out


def arm():
    man = manifest()
    st = json.loads(call('devstates'))
    print('Dev CE DLL found; registerArg check:', st.get('registerArgCheck'))
    man = [e for e in man if not hot(e)]
    for e in man:
        base = 1 + (1 if e['returns'] == 'FIXED' else 0)          # slot of the first argument (slot 0 = this; FixedPoint return = hidden buffer in slot 1)
        mask = sum(1 << (base + i) for i, a in enumerate(e['args']) if a['kind'] == 'FIXED')   # by-value FixedPoint arguments are passed by pointer
        call('stub %s %d %d %d' % (e['rva'], RET_SENTINEL[e['returns']], mask, 1 if e['returns'] == 'FIXED' else 0))
    print(call('devmarker'))
    info = json.loads(call('devarm'))
    if info['stubs'] != len([e for e in man if not hot(e)]):
        sys.exit('only %d of %d stubs installed; NOT arming' % (info['stubs'], len(man)))
    print(call('stubon 0'))
    info = json.loads(call('devarm 1'))
    print('armed:', info)
    print('Now END ONE TURN in the game. The test window runs at the start of your next turn; then run: frida_level2.py collect')


def collect():
    man = manifest()
    by_lua = {e['lua']: e for e in man}
    txt = open(LOG, encoding='utf-8', errors='replace').read().split('\n')
    last_end = max((i for i, l in enumerate(txt) if 'LEVEL2 window finished' in l or 'LEVEL2 aborted' in l), default=None)
    if last_end is None:
        sys.exit('no LEVEL2 lines in Lua.log yet: end a turn after arming')
    if 'aborted' in txt[last_end]:
        sys.exit('the script aborted the window: ' + txt[last_end])
    rows = {}
    for l in txt[:last_end]:
        m = re.search(r'L2\|([^|]+)\|([^|]*)\|(.*)$', l)
        if m:
            rows[m.group(1)] = (m.group(2), m.group(3).strip())   # later windows overwrite earlier ones
    calls = json.loads(call('stubcalls') or '{}')
    results = []
    for lua, e in by_lua.items():
        rec = calls.get(e['rva'], [])
        status, detail = None, ''
        ok_s, ret = rows.get(lua, (None, None))
        if hot(e):
            status, detail = 'EXCLUDED', 'not stub-tested: may be called by the engine from several threads'
        elif ok_s is None:
            status, detail = 'NOT_RUN', 'no L2 line for this method'
        elif ok_s == 'NOINST':
            status, detail = 'NO_INSTANCE', 'no live object of this kind in the game'
        elif ok_s != 'true':
            status, detail = 'CALL_ERROR', ret[:160]
        elif len(rec) != 1:
            status, detail = 'FAIL', 'native function entered %d times (expected 1)' % len(rec)
        else:
            r = rec[0]
            problems = []
            if r[0] in ('0',):
                problems.append('this is null')
            base = 1 + (1 if e['returns'] == 'FIXED' else 0)
            if e['returns'] == 'FIXED' and r[1] in ('0',):
                problems.append('hidden return buffer pointer is null')
            for i, v in enumerate(sentinels(e)):
                if str(v) != r[base + i]:
                    problems.append('arg%d: sent %s, native got %s' % (i, v, r[base + i]))
            if ret != RET_WANT[e['returns']]:
                problems.append('return: expected %s, Lua got %s' % (RET_WANT[e['returns']], ret))
            status, detail = ('PASS', '') if not problems else ('FAIL', '; '.join(problems))
        results.append({'lua': lua, 'rva': e['rva'], 'status': status, 'detail': detail, 'sent': sentinels(e), 'native_saw': rec[0] if len(rec) == 1 else rec})
        print('%-11s %s %s' % (status, lua, detail[:110]))
    print(call('devarm 0'))
    print(call('stubon 0'))
    json.dump(results, open(os.path.join(ROOT, 'dev-ce', 'data', 'level2_results.json'), 'w', encoding='utf-8'), indent=1)
    from collections import Counter
    print(dict(Counter(r['status'] for r in results)))


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in ('arm', 'collect'):
        sys.exit(__doc__)
    arm() if sys.argv[1] == 'arm' else collect()
