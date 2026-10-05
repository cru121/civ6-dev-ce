#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
# Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
"""Generate the Dev CE native bridge table from our analysis data.

    python dev-ce/tools/gen_bridge.py [--scope candidates|all] [--out dev-ce/src/DevNativeTable.cpp]

Inputs : docs_proto/data/function_index.json  (symbols, installed-build RVAs, Linux signatures, CE flag)
         docs_proto/data/lua_methods.json     (existing Lua methods per object, to avoid name collisions)
         dev-ce/data/candidates.json          (the 350 ranked candidates; --scope candidates)
         the installed GameCore DLL           (prologue bytes: the bridge verifies them at startup and disables a function whose bytes differ)
Outputs: dev-ce/src/DevNativeTable.cpp        generated C++ (descriptor table, thin Lua wrappers, per-interface registration hooks)
         dev-ce/data/exposed.json             manifest used by docs and tests (one entry per exposed function)
         dev-ce/data/skipped.json             why each considered function was NOT exposed

A function is exposed when ALL hold:
  * it has a Linux signature with a `this` parameter, a unique installed-build address and is not already wrapped by CE;
  * every parameter is int/uint/bool/short/char/int64-like or an enum (names ending in Types/Type): passed as one 64-bit slot, at most 7 of them;
  * the return value is void, bool or an integer/enum;
  * its class has a gameplay Lua object: the game's own ScopedInstance<I, C>::GetInstance gives `this`, and I's PushMethods/RegisterMembers is hooked
    to add the method;
  * its name does not collide with a method that object already has, and the name is not overloaded.
"""
import json, os, re, struct, sys, collections

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DEV = os.path.join(ROOT, 'dev-ce')
DLL = r"C:/Program Files (x86)/Steam/steamapps/common/Sid Meier's Civilization VI/DLC/Expansion2/Binaries/Win64/GameCore_XP2_FinalRelease.dll"
scope = sys.argv[sys.argv.index('--scope') + 1] if '--scope' in sys.argv else 'candidates'
out_cpp = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else os.path.join(DEV, 'src', 'DevNativeTable.cpp')

# ---------------------------------------------------------------- PE: rva -> file bytes
pe = open(DLL, 'rb').read()
e_lfanew = struct.unpack_from('<I', pe, 0x3c)[0]
nsec = struct.unpack_from('<H', pe, e_lfanew + 6)[0]
optsz = struct.unpack_from('<H', pe, e_lfanew + 20)[0]
secs = []
for i in range(nsec):
    o = e_lfanew + 24 + optsz + 40 * i
    vsz, va, rsz, rptr = struct.unpack_from('<IIII', pe, o + 8)
    secs.append((va, max(vsz, rsz), rptr))


def bytes_at(rva, n=8):
    for va, sz, rptr in secs:
        if va <= rva < va + sz:
            return pe[rptr + rva - va: rptr + rva - va + n]
    return None


# ---------------------------------------------------------------- data
fi = json.load(open(os.path.join(ROOT, 'docs_proto', 'data', 'function_index.json'), encoding='utf-8'))['rows']
by_name = collections.defaultdict(list)
for r in fi:
    by_name[r[0]].append(r)
GOODMAP = {'unique', 'unique-reordered', 'resolved', 'callgraph-callee', 'callgraph-caller', 'datashift-verified', 'datashift-probable', 'fuzzy-best'}


def rva_of(r):
    return int(r[2], 16) if r[2] and ',' not in r[2] and r[6] in GOODMAP else None


get_instance = {}      # class -> (iface, rva)
for r in fi:
    m = re.match(r'^Lua::Scoped(Virtual)?Instance<GameCore::Lua::(\w+),GameCore::([\w:]+)>::GetInstance$', r[0])
    if m and rva_of(r) is not None:
        get_instance.setdefault(m.group(3), []).append((m.group(2), rva_of(r), 'virtual' if m.group(1) else 'plain'))
register = {}          # iface -> (kind, rva, has_this)
for r in fi:
    m = re.match(r'^Lua::(\w+)::(PushMethods|RegisterMembers)$', r[0])
    if m and rva_of(r) is not None and r[7]:
        has_this = 'this' in r[7].split('(', 1)[1].split(',')[0]
        register[m.group(1)] = (m.group(2), rva_of(r), has_this, r[7])

ce_hooked = set()      # registration functions CE already hooks (MinHook cannot hook one address twice)
_src = os.path.join(DEV, 'src')
for _f in os.listdir(_src):
    if _f.endswith('.h'):
        _t = open(os.path.join(_src, _f), encoding='utf-8', errors='replace').read()
        for _m in re.finditer(r'(?:PUSH_METHODS|REGISTER_MEMBERS)\w*\s*=\s*(0x[0-9a-fA-F]+)', _t):
            ce_hooked.add(int(_m.group(1), 16))

lua_methods = json.load(open(os.path.join(ROOT, 'docs_proto', 'data', 'lua_methods.json'), encoding='utf-8'))
existing = collections.defaultdict(set)          # iface -> method names
objname = {}
for m in lua_methods:
    for itf in m.get('interfaces', []):
        mm = re.match(r'^Lua::(\w+)$', itf)
        if mm:
            existing[mm.group(1)].add(m['method'])
            objname[mm.group(1)] = m['object']

# Owner navigation (Windows offsets, found by call-site voting in the installed DLL: every caller of the member's functions computes `lea rcx,[owner+off]`
# before FAutoVariable::edit, or passes owner+off directly for an embedded member; see dev-ce/RESULTS.md "Owner navigation").
#   class -> (owner class, offset, mode)   mode 1 = FAutoVariable wrapper (this = edit(owner+off)), 2 = embedded (this = owner+off)
PREF = 'Lua::PlayerReference'
NAV = {'City::Trade': ('City::Instance', 0xe78, 1), 'City::CulturalIdentity': ('City::Instance', 0x1630, 2), 'City::Power': ('City::Instance', 0x1838, 1),
       'City::Culture': ('City::Instance', 0xb80, 1), 'City::Gold': ('City::Instance', 0x8a8, 1), 'City::Combat': ('City::Instance', 0xa10, 1),
       'Unit::Espionage': ('Unit::Instance', 0x8c8, 1), 'Unit::Archaeology': ('Unit::Instance', 0x888, 1),
       'Trade::Graph': ('Trade::Manager', 0xc0, 2), 'Barbarian::ClansManager': ('Barbarian::Manager', 0x1e8, 1),
       # Player root: the Lua IPlayer is a PlayerReference {id}; Player::Instance = EditPlayer(id). Components are pointer members (mode 3) or reached by
       # the game's static Get(PlayerTypes) (mode 4, offset field = rva of Get).
       'Player::Instance': (PREF, 0, 0), 'Player::Espionage': (PREF, 0x6f8, 3), 'Player::Districts': (PREF, 0x6e8, 3), 'Player::Goody_Hut': (PREF, 0x730, 3),
       'Player::CulturalIdentity': (PREF, 0x770, 3), 'Player::TurnManager': (PREF, 0x788, 3), 'Player::Bonuses': (PREF, 0x2610c0, 4), 'Player::Congress': (PREF, 0x26c770, 4)}
# Singletons: class -> (Lua interface of the static table, accessor function). `Game.X(args)` / `Map.X(args)`; this = accessor() (zero-argument, returns a reference).
SING = {}
for _c, _a in {'Game::Instance': 'Game::Instance::Edit', 'Game::Economic::Manager': 'Game::Economic::Manager::Get', 'Game::Climate::Manager': 'Game::Climate::Manager::Get',
               'Game::Culture': 'Game::Culture::Get', 'Game::Techs': 'Game::Techs::Get', 'Game::Gossip::Manager': 'Game::Gossip::Manager::Get',
               'Emergency::Manager': 'Emergency::Manager::Get', 'Player::Manager': 'Player::Manager::Edit', 'Rules::Appeal::Instance': 'Rules::Appeal::Get',
               'Rules::Economic::Instance': 'Rules::Economic::Get', 'Rules::Espionage::Instance': 'Rules::Espionage::Get', 'Map::Route::Manager': 'Map::Route::Manager::Edit'}.items():
    SING[_c] = ('IGame', _a)
for _c, _a in {'Map::Instance': 'Map::EditInstance', 'Map::Feature::Manager': 'Map::Feature::Manager::Get',
               'Map::Player::Visibility::Manager': 'Context::Globals::EditPlayerVisibility'}.items():
    SING[_c] = ('IMap', _a)
EDIT_NAME = 'FAutoVariable<Data::VariantMap,GameCore::Game::Instance>::edit'
_er = by_name[EDIT_NAME]
assert len(_er) == 1
EDIT_RVA = rva_of(_er[0])

SKIP = collections.defaultdict(list)


def skip(name, why):
    SKIP[why].append(name)


INTLIKE = {'int': 'INT', 'short': 'INT', 'char': 'INT', 'int8': 'INT', 'int16': 'INT', 'int32': 'INT', 'signed char': 'INT',
           'uint': 'UINT', 'unsigned int': 'UINT', 'ushort': 'UINT', 'uchar': 'UINT', 'uint8': 'UINT', 'uint16': 'UINT', 'uint32': 'UINT', 'unsigned short': 'UINT',
           'unsigned char': 'UINT', 'bool': 'BOOL', 'int64': 'I64', 'uint64': 'I64', 'size_t': 'I64'}


ENUMS = set()
for _l in open(os.path.join(ROOT, 'linux_depot', 'out', 'enums.jsonl'), encoding='utf-8'):
    _n = json.loads(_l)['name']
    ENUMS.add(_n.replace('GameCore::', '', 1))


def kind_of(t):
    t = re.sub(r'\b(const|volatile)\b', '', t).strip()
    if t in INTLIKE:
        return INTLIKE[t]
    if t == 'FixedPoint':
        return 'FIXED'          # FixedPointT<8>: 4-byte class, one raw int, passed by value in a register (checked by the plumbing test)
    if re.fullmatch(r'[\w:]+Types?', t) or t in ENUMS:
        return 'INT'            # enum, 32-bit
    return None


def split_params(p):
    out, depth, cur = [], 0, ''
    for ch in p:
        if ch in '<(':
            depth += 1
        if ch in '>)':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def parse_sig(sig):
    m = re.match(r'^(.*?)\((.*)\)\s*$', sig)
    if not m:
        return None
    ret, ps = m.group(1).strip(), split_params(m.group(2))
    if not ps or not re.search(r'\bthis$', ps[0]):
        return None
    args = []
    for p in ps[1:]:
        mm = re.match(r'^(.*?)\s*(\w+)$', p)
        t, n = (mm.group(1), mm.group(2)) if mm else (p, 'arg')
        k = kind_of(t)
        args.append((t.strip(), n, k))
    return ret, args


# ---------------------------------------------------------------- candidate list
if scope == 'candidates':
    cands = [('GameCore::%s::%s' % (c['class'], c['function'])).replace('GameCore::', '', 1) for c in json.load(open(os.path.join(DEV, 'data', 'candidates.json')))]
else:
    cands = sorted({r[0] for r in fi if r[7] and r[4] == 'game-logic'})

exposed = []
for qn in cands:
    rows = by_name.get(qn, [])
    if not rows:
        skip(qn, 'not in function index'); continue
    if len(rows) > 1:
        skip(qn, 'overloaded / duplicate name'); continue
    r = rows[0]
    rv = rva_of(r)
    if rv is None:
        skip(qn, 'no unique installed-build address'); continue
    if r[9]:
        skip(qn, 'already wrapped by CE'); continue
    if not r[7]:
        skip(qn, 'no Linux signature'); continue
    ps = parse_sig(r[7])
    if not ps:
        skip(qn, 'static/free function or unparsed signature'); continue
    ret, args = ps
    cls, _, fn = qn.rpartition('::')
    if fn == cls.split('::')[-1] or fn.startswith('~') or fn.startswith('operator'):
        skip(qn, 'constructor / destructor / operator (never expose)'); continue
    if cls in SING:
        _i, _an = SING[cls]
        _ar = by_name.get(_an, [])
        if len(_ar) != 1 or rva_of(_ar[0]) is None:
            skip(qn, 'singleton accessor not found'); continue
        nav, gi_special = ('Game', rva_of(_ar[0]), 5), _i
    else:
        nav, gi_special = NAV.get(cls), None
    gcls = nav[0] if nav else cls          # the class whose GetInstance gives the Lua object's native pointer
    if gi_special is None and gcls not in get_instance:
        skip(qn, 'class has no gameplay Lua object (no GetInstance)'); continue
    opts = get_instance[gcls] if gi_special is None else [(gi_special, 0, 'plain')]
    desired = 'I' + ''.join(seg for seg in gcls.split('::') if seg != 'Instance')      # Unit::Instance -> IUnit, City::Buildings -> ICityBuildings
    if gcls == PREF:
        desired = 'IPlayer'
    if gi_special:
        desired = gi_special
    pick = [o for o in opts if o[0] == desired] or (opts if len(opts) == 1 else [])
    if not pick:
        skip(qn, 'several Lua interfaces for this class, none matches the class name'); continue
    iface, gi_rva, this_kind = pick[0]
    if iface not in register:
        skip(qn, 'interface has no mapped PushMethods/RegisterMembers'); continue
    if register[iface][1] in ce_hooked:
        skip(qn, 'interface registration already hooked by CE (needs a shared hook)'); continue
    if len(args) > 7:
        skip(qn, 'more than 7 arguments'); continue
    if len(args) > 6 and ret.strip() == 'FixedPoint':
        skip(qn, 'more than 6 arguments with a hidden return buffer'); continue
    if any(a[2] is None for a in args):
        skip(qn, 'argument type not scalar (needs object/struct/float marshaling)'); continue
    rk = 'VOID' if ret == 'void' else kind_of(ret)
    if rk is None:
        skip(qn, 'return type not void/bool/integer'); continue
    if fn in existing.get(iface, ()):
        skip(qn, 'name collides with an existing Lua method'); continue
    pro, gipro = bytes_at(rv), (bytes_at(gi_rva) if gi_rva else bytes(8))
    if not pro or not gipro:
        skip(qn, 'address not inside the DLL image'); continue
    exposed.append({'this_kind': this_kind, 'qn': qn, 'cls': cls, 'fn': fn, 'rva': rv, 'iface': iface, 'obj': objname.get(iface), 'gi_rva': gi_rva, 'ret': rk, 'args': args,
                    'sig': r[7], 'pro': pro, 'gipro': gipro, 'size': r[3], 'nav': nav})

# unique Lua names per interface
seen = set()
final = []
for e in exposed:
    key = (e['iface'], e['fn'])
    if key in seen:
        skip(e['qn'], 'same Lua name as another exposed function of this object'); continue
    seen.add(key); final.append(e)
exposed = final


# ---------------------------------------------------------------- oracle entries (test builds only)
# For every Lua interface that has exposed functions, also expose up to two VANILLA getters of the same C++ class under the name DevOracle_<Name>.
# The generated self-test calls the vanilla Lua method and the bridge version and compares the results: equal answers prove that `this` and the argument
# marshaling are right for that interface (the plumbing test alone only proves that `this` is non-null). Switch off with --no-oracle (release builds).
if '--no-oracle' not in sys.argv:
    sigs = json.load(open(os.path.join(ROOT, 'docs_proto', 'data', 'lua_signatures.json'), encoding='utf-8'))
    first = {}
    for e in exposed:
        first.setdefault((e['iface'], e['cls']), e)
    n_oracle = 0
    for (itf, _c), e0 in list(first.items()):
        picks = []
        for rva_s, v in sigs.items():
            if (v.get('cpp_class') or '') != 'GameCore::Lua::' + itf or v.get('static') or not v.get('callee'):
                continue
            w = v['wrapper'][1:]
            if not re.match(r'^(Get|Is|Has|Can)[A-Z]', w) or v.get('confidence') != 'high':
                continue
            ps = v.get('params', [])
            if len(ps) > 2 or any(pp['kind'] not in ('integer', 'boolean') for pp in ps):
                continue
            if len(v.get('returns', [])) != 1 or v['returns'][0] not in ('number', 'boolean'):
                continue
            callee = v['callee']
            if callee.rpartition('::')[0] != e0['cls']:
                continue
            rows = by_name.get(callee, [])
            if len(rows) != 1 or rva_of(rows[0]) is None or not rows[0][7]:
                continue
            ps2 = parse_sig(rows[0][7])
            if not ps2:
                continue
            ret, args = ps2
            rk = kind_of(ret)
            if rk is None or any(a[2] is None for a in args) or len(args) != len(ps):
                continue
            picks.append((len(ps), w, callee, rows[0], args, rk))
        picks.sort(key=lambda t: (t[0], t[1]))
        for npar, w, callee, row, args, rk in picks[:2]:
            rv = rva_of(row)
            exposed.append({'this_kind': e0['this_kind'], 'qn': callee, 'cls': e0['cls'], 'fn': 'DevOracle_' + w, 'rva': rv, 'iface': itf, 'obj': e0['obj'],
                            'gi_rva': e0['gi_rva'], 'ret': rk, 'args': args, 'sig': row[7], 'pro': bytes_at(rv), 'gipro': e0['gipro'], 'size': row[3],
                            'oracle': True, 'vanilla': w, 'nav': e0.get('nav')})
            n_oracle += 1
    print('oracle entries added:', n_oracle)

# ---------------------------------------------------------------- C++ output
KIND = {'INT': 'A_INT', 'UINT': 'A_UINT', 'BOOL': 'A_BOOL', 'I64': 'A_I64', 'FIXED': 'A_FIXED'}
RET = {'VOID': 'R_VOID', 'BOOL': 'R_BOOL', 'INT': 'R_INT', 'UINT': 'R_UINT', 'I64': 'R_I64', 'FIXED': 'R_FIXED'}


def hexb(b):
    return ', '.join('0x%02x' % x for x in b)


L = ['// SPDX-License-Identifier: AGPL-3.0-only', '// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).', '// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.',
     '// GENERATED by dev-ce/tools/gen_bridge.py - do not edit. Installed build 15038592 (GameCore_XP2_FinalRelease.dll).',
     '#include "DevBridge.h"', '#include "Runtime.h"', '#include "ProxyTypes.h"', '#include <iostream>', '', 'namespace DevBridge {', '']
L.append('const int kFunctionCount = %d;' % len(exposed))
L.append('const FnDesc kFunctions[%d] = {' % max(1, len(exposed)))
for i, e in enumerate(exposed):
    kinds = [KIND[a[2]] for a in e['args']] + ['0'] * (7 - len(e['args']))
    L.append('\t/* %d */ { "%s", "%s", 0x%x, 0x%x, %d, { %s }, %s, { %s }, { %s }, %d, %d, %d, { %s } },' % (
        i, e['qn'], e['fn'], e['rva'], e['gi_rva'], len(e['args']), ', '.join(kinds), RET[e['ret']], hexb(e['pro']), hexb(e['gipro']),
        e['nav'][1] if e.get('nav') else 0, (0 if e['nav'][2] == 5 else e['nav'][2]) if e.get('nav') else 0, (1 if e['nav'][0] == PREF else 2 if e['nav'][2] == 5 else 0) if e.get('nav') else 0,
        hexb(bytes_at(e['nav'][1]) if e.get('nav') and e['nav'][2] in (4, 5) else bytes(8))))
if not exposed:
    L.append('\t{ "", "", 0, 0, 0, { 0 }, R_VOID, { 0 }, { 0 }, 0, 0, 0, { 0 } }')
L.append('};')
L.append('FnState gStates[%d];' % max(1, len(exposed)))
L.append('const uintptr_t kEditRva = 0x%x;   // %s' % (EDIT_RVA, EDIT_NAME))
L.append('const uint8_t kEditPrologue[8] = { %s };' % hexb(bytes_at(EDIT_RVA)))
EDITP = by_name['Context::Globals::EditPlayer']
assert len(EDITP) == 1
L.append('const uintptr_t kEditPlayerRva = 0x%x;   // Context::Globals::EditPlayer' % rva_of(EDITP[0]))
L.append('const uint8_t kEditPlayerPrologue[8] = { %s };' % hexb(bytes_at(rva_of(EDITP[0]))))
L.append('')
for i in range(len(exposed)):
    L.append('static int l_%d(hks::lua_State* L) { return Dispatch(L, %d); }' % (i, i))
L.append('')
by_iface = collections.defaultdict(list)
for i, e in enumerate(exposed):
    by_iface[e['iface']].append(i)
ifaces = sorted(by_iface)
for k, itf in enumerate(ifaces):
    kind, rva, has_this, sig = register[itf]
    Lname = 'b' if has_this else 'a'
    tname = 'c' if has_this else 'b'
    L.append('// %s::%s  %s' % (itf, kind, sig))
    L.append('static void(__cdecl* base_reg_%d)(void*, void*, void*);' % k)
    L.append('static void __cdecl hook_reg_%d(void* a, void* b, void* c) {' % k)
    L.append('\thks::lua_State* L = (hks::lua_State*)%s;' % Lname)
    if kind == 'PushMethods':
        L.append('\tint t = (int)(intptr_t)%s;' % tname)
    else:
        L.append('\tint t = -2;')
    for i in by_iface[itf]:
        e = exposed[i]
        L.append('\tif (gStates[%d].ok) { PushLuaMethod(L, l_%d, "lDev%s", t, "%s"); }' % (i, i, e['fn'], e['fn']))
    L.append('\tLog("[DevBridge] %s: %d methods added to the Lua object", "%s", %d);' % (itf, len(by_iface[itf]), itf, len(by_iface[itf])))
    L.append('\tbase_reg_%d(a, b, c);' % k)
    L.append('}')
L.append('')
L.append('struct RegDesc { const char* iface; uintptr_t rva; uint8_t prologue[8]; void* hook; void** base; };')
L.append('static const RegDesc kRegs[] = {')
for k, itf in enumerate(ifaces):
    kind, rva, has_this, sig = register[itf]
    L.append('\t{ "%s", 0x%x, { %s }, (void*)&hook_reg_%d, (void**)&base_reg_%d },' % (itf, rva, hexb(bytes_at(rva)), k, k))
if not ifaces:
    L.append('\t{ "", 0, { 0 }, 0, 0 }')
L.append('};')
L.append('')
L.append('void InstallGeneratedHooks() {')
L.append('\tfor (const RegDesc& r : kRegs) {')
L.append('\t\tif (!r.rva) continue;')
L.append('\t\tuint8_t* p = (uint8_t*)(Runtime::GameCoreAddress + r.rva);')
L.append('\t\tif (memcmp(p, r.prologue, 8) != 0) { Log("[DevBridge] registration function of %s has unexpected bytes: its methods are not added", r.iface); continue; }')
L.append('\t\tRuntime::CreateHook((LPVOID)p, (LPVOID)r.hook, (void**)r.base);')
L.append('\t}')
L.append('}')
L.append('')
L.append('} // namespace DevBridge')
os.makedirs(os.path.dirname(out_cpp), exist_ok=True)
open(out_cpp, 'w', encoding='utf-8').write('\n'.join(L) + '\n')

manifest = [{'lua': '%s.%s' % (e['obj'] or e['iface'], e['fn']), 'iface': e['iface'], 'cpp': 'GameCore::' + e['qn'], 'rva': hex(e['rva']),
             'this_kind': e['this_kind'], 'oracle': e.get('oracle', False), 'vanilla': e.get('vanilla'), 'signature': e['sig'], 'args': [{'name': a[1], 'type': a[0], 'kind': a[2]} for a in e['args']], 'returns': e['ret'], 'index': i,
             'nav': ({'owner': e['nav'][0], 'offset': hex(e['nav'][1]), 'mode': {0: 'itself', 1: 'FAutoVariable', 2: 'embedded', 3: 'pointer', 4: 'static Get(player)', 5: 'static Game/Map accessor'}[e['nav'][2]]} if e.get('nav') else None)}
            for i, e in enumerate(exposed)]
json.dump(manifest, open(os.path.join(DEV, 'data', 'exposed.json'), 'w', encoding='utf-8'), indent=1)
json.dump({k: v for k, v in SKIP.items()}, open(os.path.join(DEV, 'data', 'skipped.json'), 'w', encoding='utf-8'), indent=1)
print('scope', scope, '| considered', len(cands), '| exposed', len(exposed), '| interfaces hooked', len(ifaces))
for k, v in sorted(SKIP.items(), key=lambda kv: -len(kv[1])):
    print('  skipped %4d  %s' % (len(v), k))
