#!/usr/bin/env python3
"""Reads the level-3 (delta probe) lines from DevBridge.log and writes dev-ce/data/level3_results.json plus a printed summary.

    python dev-ce/tools/level3_report.py [path to DevBridge.log]
Line format:  L3|<Object.Method>|plus=<bool>|minus=<bool>|changed=<n>|restored=yes|NO:<n>|diff=<k:v->v;...>[|stuck=<...>]
"""
import collections, json, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DEFAULT = os.path.expandvars(r"%USERPROFILE%\OneDrive\Documents\My Games\Sid Meier's Civilization VI\Mods\DevCE_Test\Binaries\Win64\DevBridge.log")
path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
rows, begin = [], None
for line in open(path, encoding='utf-8', errors='replace'):
    line = line.rstrip('\n')
    if not line.startswith('L3|'):
        continue
    p = line.split('|')
    if p[1] == 'BEGIN':
        begin = p[2]
    elif p[1] in ('START', 'END'):
        continue
    elif len(p) >= 3 and p[2] == 'NOINST':
        rows.append({'lua': p[1], 'status': 'NOINST'})
    else:
        d = {k: v for k, v in (x.split('=', 1) for x in p[2:] if '=' in x)}
        diff = [x for x in d.get('diff', '').split(';') if x]
        rows.append({'lua': p[1], 'plus': d.get('plus') == 'true', 'minus': d.get('minus') == 'true', 'changed': int(d.get('changed', 0)),
                     'restored': d.get('restored', ''), 'diff': diff, 'stuck': d.get('stuck', '')})
        begin = None
if begin:
    print('LAST BEGIN WITHOUT RESULT (probable crash culprit):', begin)
json.dump(rows, open(os.path.join(ROOT, 'dev-ce', 'data', 'level3_results.json'), 'w', encoding='utf-8'), indent=1)
c = collections.Counter()
for r in rows:
    if r.get('status') == 'NOINST':
        c['no instance'] += 1; continue
    c['call errors (plus or minus)'] += (not r['plus']) + (not r['minus'])
    c['visible effect (getters changed)' if r['changed'] else 'no visible effect via vanilla getters'] += 1
    c['restored by -1' if r['restored'] == 'yes' else 'NOT restored'] += 1
print(dict(c))
print()
for r in rows:
    if r.get('status') == 'NOINST':
        print('NOINST  ', r['lua']); continue
    flag = ('ERR ' if not (r['plus'] and r['minus']) else '    ') + ('CHANGED ' if r['changed'] else 'silent  ') + ('restored' if r['restored'] == 'yes' else 'STUCK(' + r['restored'] + ')')
    print('%s  %-62s %s' % (flag, r['lua'], '; '.join(r['diff'][:3])[:110]))
