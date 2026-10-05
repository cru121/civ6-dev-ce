#!/usr/bin/env python3
"""Prepare input files for the decompile-summary agents.

Reads  : gap_shortlist.tsv (350 ranked candidates), tools/gap_decomp.txt (Ghidra decompilation of all gap functions, key = old rva),
         linux_depot/out/functions.tsv (Linux DWARF: typed parameter NAMES), docs_proto/data/lua_signatures.json (classes with a Lua object)
Writes : dev-ce/data/candidates.json            all 350 with flags {this_resolvable, scalar_only, tier_hint}
         dev-ce/batch/inputs/batch_NN.md         ~24 functions per file (signature, facts, decompiled C), first the directly callable ones
Run    : python dev-ce/tools/prep_batch.py [--per 24]
"""
import csv, json, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OUT = os.path.join(ROOT, 'dev-ce')
PER = int(sys.argv[sys.argv.index('--per') + 1]) if '--per' in sys.argv else 24
MAXC = 9000      # characters of decompiled C per function given to an agent

rows = list(csv.DictReader(open(os.path.join(ROOT, 'gap_shortlist.tsv'), encoding='utf-8').read().split('\n')[1:], delimiter='\t'))
sig = json.load(open(os.path.join(ROOT, 'docs_proto', 'data', 'lua_signatures.json')))
lua_classes = {v['callee'].rsplit('::', 1)[0] for v in sig.values() if v.get('callee') and '::' in v['callee']}

# decompilation by old rva
dec = {}
cur = None
for line in open(os.path.join(ROOT, 'tools', 'gap_decomp.txt'), encoding='utf-8', errors='replace'):
    if line.startswith('@@@ '):
        parts = line[4:].rstrip('\n').split('\t')
        cur = parts[0].strip()
        dec[cur] = {'hdr': parts, 'c': []}
    elif cur:
        dec[cur]['c'].append(line)

# Linux typed params by qualified name
lin = {}
for line in open(os.path.join(ROOT, 'linux_depot', 'out', 'functions.tsv'), encoding='utf-8', errors='replace'):
    p = line.rstrip('\n').split('\t')
    if len(p) >= 6:
        lin.setdefault(p[3], []).append((p[4], p[5]))

SCAL = r'(int|uint|bool|float|double|short|ushort|char|uchar|long|ulong|int64|uint64|size_t)'


def scalar_only(signature):
    m = re.match(r'^(.*?)\((.*)\)\s*$', signature)
    if not m:
        return False
    ps = [p.strip() for p in re.split(r',\s*(?![^<]*>)', m.group(2)) if p.strip()]
    for p in (ps[1:] if ps and 'this' in ps[0] else ps):
        t = re.sub(r'\s+\w+$', '', re.sub(r'\b(const|volatile)\b', '', p).strip())
        if not (re.fullmatch(SCAL, t) or re.fullmatch(r'\w+Types?', t)):
            return False
    return True


cands = []
for i, r in enumerate(rows):
    cls = r['class']
    this_ok = cls in lua_classes or ('GameCore::' + cls) in lua_classes
    sc = scalar_only(r['signature'])
    cands.append({'rank': i + 1, 'old_rva': r['old_rva'], 'new_rva': r['new_rva'], 'class': cls, 'function': r['function'], 'signature': r['signature'],
                  'size': r['size'], 'callers': r['callers'], 'this_resolvable': this_ok, 'scalar_only': sc,
                  'tier_hint': 'direct' if (this_ok and sc) else ('this-ok-needs-object-args' if this_ok else 'no-lua-object-for-this-class')})
os.makedirs(os.path.join(OUT, 'data'), exist_ok=True)
json.dump(cands, open(os.path.join(OUT, 'data', 'candidates.json'), 'w', encoding='utf-8'), indent=1)

order = [c for c in cands if c['tier_hint'] == 'direct'] + [c for c in cands if c['tier_hint'] != 'direct']
inp = os.path.join(OUT, 'batch', 'inputs')
os.makedirs(inp, exist_ok=True)
for f in os.listdir(inp):
    os.remove(os.path.join(inp, f))
facts_rows = {r['old_rva']: r for r in rows}
nb = 0
for b in range(0, len(order), PER):
    chunk = order[b:b + PER]
    nb += 1
    with open(os.path.join(inp, 'batch_%02d.md' % nb), 'w', encoding='utf-8') as f:
        f.write('# Batch %02d: %d functions (tier_hint of the first: %s)\n\n' % (nb, len(chunk), chunk[0]['tier_hint']))
        for c in chunk:
            d = dec.get(c['old_rva'])
            r = facts_rows[c['old_rva']]
            qn = 'GameCore::%s::%s' % (c['class'], c['function'])
            f.write('=' * 100 + '\n')
            f.write('## %s\nold_rva %s | installed-build rva %s | size %s bytes | direct callers %s | tier_hint %s\n' % (qn, c['old_rva'], c['new_rva'], c['size'], c['callers'], c['tier_hint']))
            f.write('Ghidra signature: %s\n' % c['signature'])
            for (params, ret) in lin.get(qn, [])[:3]:
                f.write('Linux DWARF (names are real): %s | returns %s\n' % (params, ret))
            f.write('script facts: event=%s signal=%s notification=%s edit()=%s lock=%s writes=%s random=%s uses_turn=%s vcalls=%s offsets=%s\n' % (
                r.get('event'), r.get('signal'), r.get('notification'), r.get('edit_call'), r.get('lock'), r.get('writes'), r.get('random'), r.get('uses_turn'), r.get('vcalls'), r.get('offsets')))
            f.write('callees: %s\n' % (r.get('callees') or '')[:900])
            f.write('--- decompiled C (Ghidra; local names are generic) ---\n')
            if d:
                text = ''.join(d['c'])
                f.write(text[:MAXC] + ('\n[... truncated ...]\n' if len(text) > MAXC else '\n'))
            else:
                f.write('(no decompilation available)\n')
print('candidates', len(cands), 'direct', sum(1 for c in cands if c['tier_hint'] == 'direct'), 'batches', nb)
