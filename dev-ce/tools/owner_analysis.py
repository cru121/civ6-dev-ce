#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
# Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
"""For every class that has exposed-worthy functions but no Lua object (skipped: 'class has no gameplay Lua object'), find which other class holds it as a member
(from the Linux DWARF types), and whether that owner has a Lua object whose `this` we can already get. Output: dev-ce/data/owner_candidates.json and a printed table.

A member counts when its type string mentions the target class (value, pointer, unique_ptr<...>, FAutoVariable<...>). We keep the owner offsets as LINUX offsets:
Windows offsets still need the check (rule of thumb from earlier work: minus 8 for FAutoVariable-heavy classes, same for others; must be verified at run time).
"""
import collections, json, os, re

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DEV = os.path.join(ROOT, 'dev-ce')
sk = json.load(open(os.path.join(DEV, 'data', 'skipped.json'), encoding='utf-8'))
targets = collections.Counter(q.rsplit('::', 1)[0] for q in sk['class has no gameplay Lua object (no GetInstance)'])
exposed = json.load(open(os.path.join(DEV, 'data', 'exposed.json'), encoding='utf-8'))
have_this = {}   # class -> (iface, kind) for classes whose `this` the bridge can get
fi = json.load(open(os.path.join(ROOT, 'docs_proto', 'data', 'function_index.json'), encoding='utf-8'))['rows']
for r in fi:
    m = re.match(r'^Lua::Scoped(Virtual)?Instance<GameCore::Lua::(\w+),GameCore::([\w:]+)>::GetInstance$', r[0])
    if m and r[2] and ',' not in r[2]:
        have_this.setdefault(m.group(3), (m.group(2), 'virtual' if m.group(1) else 'plain'))

# DWARF types: owner -> members
types = {}
with open(os.path.join(ROOT, 'linux_depot', 'out', 'types.jsonl'), encoding='utf-8') as f:
    for line in f:
        t = json.loads(line)
        if t.get('members') and t['name'].startswith('GameCore::'):
            types[t['name'][len('GameCore::'):]] = t

rows = []
for tgt, n in targets.most_common():
    pat = re.compile(r'(?<![\w:])(GameCore::)?' + re.escape(tgt) + r'(?![\w:])')
    holders = []
    for owner, t in types.items():
        for m in t['members']:
            name, off, ty = m[0], m[1], m[2]
            if ty and pat.search(ty):
                holders.append((owner, name, off, ty[:80]))
    # prefer owners we can already reach
    reach = [h for h in holders if h[0] in have_this]
    rows.append({'class': tgt, 'functions': n, 'owners_reachable': reach[:4], 'owners_other': [h for h in holders if h[0] not in have_this][:4]})
json.dump(rows, open(os.path.join(DEV, 'data', 'owner_candidates.json'), 'w', encoding='utf-8'), indent=1)
n_reach = sum(r['functions'] for r in rows if r['owners_reachable'])
print('classes %d, functions %d; functions whose class is a member of a reachable owner: %d' % (len(rows), sum(r['functions'] for r in rows), n_reach))
for r in rows:
    o = r['owners_reachable'][0] if r['owners_reachable'] else None
    print('%-42s %3d  %s' % (r['class'], r['functions'], ('%s.%s @0x%x  (%s)  [%s %s]' % (o[0], o[1], o[2], o[3][:40], *have_this[o[0]])) if o else 'NO reachable owner; other: %s' % [(h[0], h[1]) for h in r['owners_other'][:2]]))
