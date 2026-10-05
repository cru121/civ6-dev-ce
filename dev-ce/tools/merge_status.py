#!/usr/bin/env python3
"""Merge the test results of all runs into dev-ce/data/function_status.json (best evidence wins: an effect confirmed in one run is kept even if another game state shows nothing).

    python dev-ce/tools/merge_status.py            # level 2 from level2_results.json, level 3 from level3_results.json + level3_history.json
Level-3 history: dev-ce/data/level3_history.json is a list of runs {date, results:[{lua, changed, restored, diff}]}; this script appends the current level3_results.json
(if not yet recorded for that date label) with --record LABEL.
"""
import json, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
D = os.path.join(ROOT, 'dev-ce', 'data')
man = json.load(open(os.path.join(D, 'exposed.json'), encoding='utf-8'))
l2 = {r['lua']: r['status'] for r in json.load(open(os.path.join(D, 'level2_results.json'), encoding='utf-8'))}
hp = os.path.join(D, 'level3_history.json')
hist = json.load(open(hp, encoding='utf-8')) if os.path.exists(hp) else []
cur = json.load(open(os.path.join(D, 'level3_results.json'), encoding='utf-8')) if os.path.exists(os.path.join(D, 'level3_results.json')) else []
if '--record' in sys.argv:
    label = sys.argv[sys.argv.index('--record') + 1]
    hist = [h for h in hist if h['label'] != label]
    hist.append({'label': label, 'results': [r for r in cur if r.get('status') != 'NOINST']})
    json.dump(hist, open(hp, 'w', encoding='utf-8'), indent=1)

best = {}   # lua -> merged delta_probe
for h in hist:
    for r in h['results']:
        names = [re.sub(r'^\d+\.', '', x.split(':')[0]) for x in r.get('diff', [])]
        cand = {'visible_effect': r.get('changed', 0) > 0 if 'changed' in r else r.get('visible_effect', False),
                'reversible': (r.get('restored') == 'yes') if 'restored' in r else r.get('reversible', True),
                'changed_getters': names or r.get('changed_getters', []), 'runs': 1}
        b = best.get(r['lua'])
        if not b:
            best[r['lua']] = cand
        else:
            b['runs'] += 1
            if cand['visible_effect'] and not b['visible_effect']:
                b.update(visible_effect=True, changed_getters=cand['changed_getters'])
            # reversible only if every run restored
            b['reversible'] = b['reversible'] and cand['reversible']
st = {}
for e in man:
    s = {'plumbing': l2.get(e['lua'], 'not-run'), 'this_kind': e['this_kind']}
    if e['lua'] in best:
        s['delta_probe'] = best[e['lua']]
    st[e['lua']] = s
json.dump(st, open(os.path.join(D, 'function_status.json'), 'w', encoding='utf-8'), indent=1)
print('functions', len(st), '| plumbing PASS', sum(1 for v in st.values() if v['plumbing'] == 'PASS'), '| delta probed', sum(1 for v in st.values() if 'delta_probe' in v),
      '| effect visible', sum(1 for v in st.values() if v.get('delta_probe', {}).get('visible_effect')), '| not reversible', [k for k, v in st.items() if v.get('delta_probe') and not v['delta_probe']['reversible']])
