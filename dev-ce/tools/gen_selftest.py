#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
# Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
"""Generate the Dev CE test mod (dev-ce/mod/DevCE_Test): modinfo, Config.sql and a gameplay self-test script from data/exposed.json.

The self-test (level 1, presence): at the first turn start of the human player it walks to one live instance of every Lua object kind that has
exposed methods and checks by reflection that each generated method is really there. Results go to Lua.log (search for "DevCE").
Objects it cannot reach from the player/first city/first unit are listed as NOT REACHED. Nothing is called at this level.
"""
import json, os, re, shutil, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DEV = os.path.join(ROOT, 'dev-ce')
MOD = os.path.join(DEV, 'mod', 'DevCE_Test')
GUID = '6b0d0b1c-7f61-4d6e-9d0a-3f2f5c1c7d11'

ACCESS = {   # Lua interface -> expression giving one live instance in the gameplay state (p = Players[0], capital, unit are set up by the script)
    'ICityBuildings': 'capital:GetBuildings()', 'ICityBuildQueue': 'capital:GetBuildQueue()', 'ICityDistricts': 'capital:GetDistricts()',
    'ICityGrowth': 'capital:GetGrowth()', 'IUnit': 'unit', 'IUnitExperience': 'unit:GetExperience()', 'IUnitGreatPerson': 'unit:GetGreatPerson()',
    'IUnitReligion': 'unit:GetReligion()', 'IPlayerUnits': 'p:GetUnits()', 'IPlayerTechs': 'p:GetTechs()',
    'IPlayerGreatPeoplePoints': 'p:GetGreatPeoplePoints()', 'IPlayerReligion': 'p:GetReligion()', 'IPlayerTreasury': 'p:GetTreasury()',
    'IGameGreatPeople': 'Game.GetGreatPeople()', 'IGameEras': 'Game.GetEras()', 'IGameDiplomacy': 'Game.GetGameDiplomacy()',
    'IPlayerCulture': 'p:GetCulture()', 'IPlayerDiplomacy': 'p:GetDiplomacy()', 'IPlayerResources': 'p:GetResources()', 'IPlayerTrade': 'p:GetTrade()',
    'ICity': 'capital', 'IPlayer': 'p', 'IGame': 'DevStatic(Game)', 'IMap': 'DevStatic(Map)', 'ITradeManager': 'Game.GetTradeManager()', 'IBarbarianTribes': 'Game.GetBarbarianManager()', 'IPlayerStats': 'p:GetStats()',
}

man = json.load(open(os.path.join(DEV, 'data', 'exposed.json'), encoding='utf-8'))
by = {}
for e in man:
    by.setdefault(e['iface'], []).append(e)


def sentinel_lua(kind, i):
    return {'INT': str(11 + i), 'UINT': str(21 + i), 'I64': str(31 + i), 'BOOL': 'true', 'FIXED': '%d.5' % (2 + i)}[kind]


L2LINES = ['local LEVEL2 = {']
for e in man:
    if e['iface'] not in ACCESS:
        continue
    if e.get('nav') and e['nav']['owner'] in ('Lua::PlayerReference', 'Game'):
        continue   # never stub-tested (engine-hot risk, see frida_level2.py hot())
    obj, meth = e['lua'].split('.', 1)
    args = ', '.join(sentinel_lua(a['kind'], i) for i, a in enumerate(e['args']))
    L2LINES.append('    { name = "%s", reach = function(p, capital, unit) return %s end, run = function(o) return o:%s(%s) end },' % (e['lua'], ACCESS[e['iface']], meth, args))
L2LINES += ['}', '',
            '-- Level 2 (plumbing): only runs while the Frida test tool has armed the DLL. The tool replaces the native functions by recording stubs, so',
            '-- nothing happens in the game; the markers (1 = begin, 2 = end) tell the tool when to record. Results are logged after the window.',
            'local function Level2()',
            '    local p = Players[0]',
            '    local capital, unit',
            '    pcall(function() capital = p:GetCities():GetCapitalCity() end)',
            '    pcall(function() for _, u in p:GetUnits():Members() do unit = u break end end)',
            '    local lines = {}',
            '    DevCE_RecordState(1)',
            '    if DevCE_IsArmed() ~= 2 then',
            '        DevCE_RecordState(2)',
            '        Log("LEVEL2 aborted: the test tool did not confirm that the native functions are stubbed; NOTHING was called")',
            '        return',
            '    end',
            '    for _, t in ipairs(LEVEL2) do',
            '        local ok0, o = pcall(t.reach, p, capital, unit)',
            '        if not ok0 or o == nil then',
            '            lines[#lines + 1] = "L2|" .. t.name .. "|NOINST|"',
            '        else',
            '            local ok, r = pcall(t.run, o)',
            '            lines[#lines + 1] = "L2|" .. t.name .. "|" .. tostring(ok) .. "|" .. tostring(r)',
            '        end',
            '    end',
            '    DevCE_RecordState(2)',
            '    for _, l in ipairs(lines) do Log(l) end',
            '    Log("LEVEL2 window finished: " .. #lines .. " calls")',
            'end', '']


ORACLE_LINES = ['local ORACLE = {']
for e in man:
    if not e.get('oracle') or e['iface'] not in ACCESS:
        continue
    argl = ', '.join('false' if a['kind'] == 'BOOL' else '0' for a in e['args'])
    ORACLE_LINES.append('    { name = "%s", reach = function(p, capital, unit) return %s end, v = function(o) return o:%s(%s) end, d = function(o) return o:%s(%s) end },' % (
        e['lua'].split('.', 1)[0] + '.' + e['vanilla'], ACCESS[e['iface']], e['vanilla'], argl, e['lua'].split('.', 1)[1], argl))
ORACLE_LINES += ['}', '',
                 '-- Oracle test: the vanilla Lua getter and the bridge-exposed engine function of the same class must give the same answer.',
                 'local function Oracle()',
                 '    local p = Players[0]',
                 '    local capital, unit',
                 '    pcall(function() capital = p:GetCities():GetCapitalCity() end)',
                 '    pcall(function() for _, u in p:GetUnits():Members() do unit = u break end end)',
                 '    local ok, bad, err, noinst = 0, 0, 0, 0',
                 '    for _, t in ipairs(ORACLE) do',
                 '        local ok0, o = pcall(t.reach, p, capital, unit)',
                 '        if not ok0 or o == nil then noinst = noinst + 1; Log("ORACLE|" .. t.name .. "|NOINST")',
                 '        else',
                 '            local a, va = pcall(t.v, o)',
                 '            local b, vb = pcall(t.d, o)',
                 '            if a and b then',
                 '                if tostring(va) == tostring(vb) then ok = ok + 1; Log("ORACLE|" .. t.name .. "|OK|" .. tostring(va))',
                 '                else bad = bad + 1; Log("ORACLE|" .. t.name .. "|MISMATCH|vanilla=" .. tostring(va) .. "|bridge=" .. tostring(vb)) end',
                 '            else err = err + 1; Log("ORACLE|" .. t.name .. "|ERROR|" .. tostring(a) .. ":" .. tostring(va) .. "|" .. tostring(b) .. ":" .. tostring(vb)) end',
                 '        end',
                 '    end',
                 '    Log(string.format("ORACLE summary: %d match, %d mismatch, %d error, %d no instance", ok, bad, err, noinst))',
                 'end', '']


# ---------------------------------------------------------------- level 3: delta probe (real execution, reversible Change* functions)
import csv, glob


def load_agent():
    ag = {}
    for f in glob.glob(os.path.join(DEV, 'batch', 'out', 'batch_*.jsonl')):
        for l in open(f, encoding='utf-8'):
            if l.strip():
                o = json.loads(l)
                ag[o['function'].replace('GameCore::', '', 1)] = o
    return ag


STATIC_ROOTS = ('Game', 'Map', 'Players', 'Cities', 'Units', 'Calendar', 'PlayerManager')
SNAP = {}
ct = os.path.join(ROOT, 'frida', 'live', 'luatests', 'calltest_results.tsv')
if os.path.exists(ct):
    for r in csv.DictReader(open(ct, encoding='utf-8'), delimiter='\t'):
        if r['verdict'] != 'ok-count-match':
            continue
        if r['runtime_returns'] not in ('1:number', '1:boolean'):
            continue
        SNAP.setdefault(r['path'], []).append(r['method'])


def snap_expr(path):
    if path in STATIC_ROOTS:
        return path, True
    e = path
    if e.startswith('Players[0]'):
        e = 'p' + e[len('Players[0]'):]
    elif e.startswith('firstUnit'):
        e = 'unit' + e[len('firstUnit'):]
    return e, False


L3LINES = ['local SNAP = {']
for path, methods in sorted(SNAP.items()):
    expr, static = snap_expr(path)
    L3LINES.append('    { static = %s, get = function(p, capital, unit, plot0) return %s end, methods = { %s } },' % (
        'true' if static else 'false', expr, ', '.join('"%s"' % m for m in sorted(set(methods)))))
L3LINES += ['}', '']

DELTA_RE = re.compile(r'^(i|x)?(Change|Delta|Amount|ModDelta|RangeDelta|PopulationAmount|amount)$')
ag = load_agent()
L3_TARGETS = []
# Not run in level 3: Unit.ChangeSightRange hung the game (thread dump: game thread waiting, no fault) on a late-game save 2026-10-05 21:58
# and has lasting side effects (reveals tiles, meets city states) even when it works.
L3_SKIP = {'Unit.ChangeSightRange'}
for e in man:
    if e.get('oracle') or e['iface'] not in ACCESS:
        continue
    fn = e['lua'].split('.', 1)[1]
    if e['lua'] in L3_SKIP:
        continue
    if not fn.startswith('Change') or any(a['kind'] not in ('INT', 'UINT', 'I64', 'FIXED') for a in e['args']):
        continue
    if any('Hash' in a['name'] for a in e['args']):
        continue
    a = ag.get(e['cpp'].replace('GameCore::', '', 1))
    if not a or a['risk'] == 'high' or a['confidence'] == 'low':
        continue
    names = [x['name'] for x in e['args']]
    dpos = [i for i, n in enumerate(names) if DELTA_RE.match(n)]
    if not dpos:
        continue
    di = dpos[-1]
    L3_TARGETS.append((e, di))

L3LINES.append('local LEVEL3 = {')
for e, di in L3_TARGETS:
    def argl(sign):
        return ', '.join(('%d' % sign) if i == di else '0' for i, a in enumerate(e['args']))
    L3LINES.append('    { name = "%s", reach = function(p, capital, unit) return %s end, plus = function(o) return o:%s(%s) end, minus = function(o) return o:%s(%s) end },' % (
        e['lua'], ACCESS[e['iface']], e['lua'].split('.', 1)[1], argl(1), e['lua'].split('.', 1)[1], argl(-1)))
L3LINES += ['}', '']
L3LINES += [
    'local function LogF(msg) if DevCE_Log then DevCE_Log(msg) else Log(msg) end end',
    'local function Snapshot(p, capital, unit, plot0)',
    '    local s = {}',
    '    for i, grp in ipairs(SNAP) do',
    '        local ok, o = pcall(grp.get, p, capital, unit, plot0)',
    '        if ok and o ~= nil then',
    '            for _, m in ipairs(grp.methods) do',
    '                local fnc = o[m]',
    '                if fnc ~= nil then',
    '                    local ok2, v',
    '                    if grp.static then ok2, v = pcall(fnc) else ok2, v = pcall(fnc, o) end',
    '                    if ok2 then s[i .. "." .. m] = tostring(v) end',
    '                end',
    '            end',
    '        end',
    '    end',
    '    return s',
    'end',
    'local function Diff(a, b)',
    '    local d = {}',
    '    for k, v in pairs(a) do if b[k] ~= v then d[#d + 1] = k .. ":" .. v .. "->" .. tostring(b[k]) end end',
    '    table.sort(d)',
    '    return d',
    'end',
    '-- Level 3 (real execution on a disposable game): for each reversible Change* function: snapshot ~' + str(sum(len(v) for v in SNAP.values())) + ' vanilla getters, call with +1, snapshot,',
    '-- call with -1, snapshot. Runs once when the tool/user sets DevBridgeArmed to 4. Every step is written to DevBridge.log first (flushed): after a crash the last',
    '-- "L3|BEGIN" line names the function that did it.',
    'local function Level3()',
    '    local p = Players[0]',
    '    local capital, unit, plot0',
    '    pcall(function() capital = p:GetCities():GetCapitalCity() end)',
    '    pcall(function() for _, u in p:GetUnits():Members() do unit = u break end end)',
    '    pcall(function() plot0 = Map.GetPlotByIndex(0) end)',
    '    DevCE_RecordState(5)',
    '    LogF("L3|START|" .. #LEVEL3 .. " functions")',
    '    for _, t in ipairs(LEVEL3) do',
    '        local ok0, o = pcall(t.reach, p, capital, unit)',
    '        if not ok0 or o == nil then',
    '            LogF("L3|" .. t.name .. "|NOINST")',
    '        else',
    '            LogF("L3|BEGIN|" .. t.name)',
    '            local s0 = Snapshot(p, capital, unit, plot0)',
    '            local okp, rp = pcall(t.plus, o)',
    '            local s1 = Snapshot(p, capital, unit, plot0)',
    '            local okm, rm = pcall(t.minus, o)',
    '            local s2 = Snapshot(p, capital, unit, plot0)',
    '            local d1, d2 = Diff(s0, s1), Diff(s0, s2)',
    '            LogF("L3|" .. t.name .. "|plus=" .. tostring(okp) .. "|minus=" .. tostring(okm) .. "|changed=" .. #d1 .. "|restored=" .. (#d2 == 0 and "yes" or "NO:" .. #d2) .. "|diff=" .. table.concat(d1, ";", 1, math.min(#d1, 6)) .. (#d2 > 0 and "|stuck=" .. table.concat(d2, ";", 1, math.min(#d2, 4)) or ""))',
    '        end',
    '    end',
    '    LogF("L3|END")',
    'end', '']

# ---------------------------------------------------------------- level 4: hostile arguments (robustness; nothing here is meant to succeed)
# Cases that could reach the native function with a valid `this` always pass "x" for every argument, so the call stops at the argument check
# (checkinteger/checknumber raise) before any native code runs; this only works when the function has a non-bool argument (`guarded`).
# Cases with an INVALID self (nil, string, number, table, an object of another kind) test GetInstance's type check; for functions without a
# guard those would run natively if the check wrongly passed, so they come last (section B).
# Value-hostile calls (-1, INT_MAX, INT_MIN, 1e10, 0.5, NaN) are only made on the read-only oracle getters.
def lua_bool(b):
    return 'true' if b else 'false'


L4LINES = ['local LEVEL4 = {']
for e in man:
    if e['iface'] not in ACCESS:
        continue
    static = bool(e.get('nav') and e['nav'].get('owner') in ('Game', 'Map')) or e['iface'] in ('IGame', 'IMap')
    obj, meth = e['lua'].split('.', 1)
    guarded = any(a['kind'] != 'BOOL' for a in e['args'])
    acc = re.sub(r'^DevStatic\((\w+)\)$', r'\g<1>', ACCESS[e['iface']])
    L4LINES.append('    { name = "%s", reach = function(p, capital, unit) return %s end, meth = "%s", static = %s, nargs = %d, guarded = %s, getter = %s },' % (
        e['lua'], acc, meth, lua_bool(static), len(e['args']), lua_bool(guarded), lua_bool(bool(e.get('oracle')))))
L4LINES += ['}', '']
L4LINES += [
    'local function Short(v) local s = tostring(v); s = string.gsub(s, "[\\r\\n]+", " "); if #s > 90 then s = string.sub(s, 1, 90) end; return s end',
    'local unpack = unpack or table.unpack',
    'local function Xs(n) local t = {}; for i = 1, n do t[i] = "x" end; return t end',
    '-- Level 4 (hostile arguments): armed code 6. Every call is under pcall; the outcome is logged line by line (flushed). A hang or crash: the last "L4|BEGIN" line names the function.',
    '-- ERR = the call raised a Lua error (expected for bad input); OK = it returned. For the "must fail" cases OK is reported as UNEXPECTED_OK.',
    'local function Level4()',
    '    local p = Players[0]',
    '    local capital, unit',
    '    pcall(function() capital = p:GetCities():GetCapitalCity() end)',
    '    pcall(function() for _, u in p:GetUnits():Members() do unit = u break end end)',
    '    DevCE_RecordState(7)',
    '    local stat = { err = 0, ok = 0, unexpected = 0, fault = 0 }',
    '    local function Case(name, case, mustFail, f, ...)',
    '        local ok, r = pcall(f, ...)',
    '        local res',
    '        if ok then res = mustFail and "UNEXPECTED_OK" or "OK"; stat.ok = stat.ok + 1; if mustFail then stat.unexpected = stat.unexpected + 1 end',
    '        else res = "ERR"; stat.err = stat.err + 1; if string.find(tostring(r), "faulted", 1, true) then res = "FAULT"; stat.fault = stat.fault + 1 end end',
    '        LogF("L4|" .. name .. "|" .. case .. "|" .. res .. "|" .. Short(r))',
    '    end',
    '    LogF("L4|START|" .. #LEVEL4 .. " functions")',
    '    -- section A: functions with a non-bool argument: every case passes "x" for all arguments, so the native function is never reached',
    '    for _, t in ipairs(LEVEL4) do',
    '        if t.guarded then',
    '            local ok0, o = pcall(t.reach, p, capital, unit)',
    '            if ok0 and o ~= nil then',
    '                LogF("L4|BEGIN|" .. t.name)',
    '                local f = o[t.meth]',
    '                local xs = Xs(t.nargs)',
    '                if f == nil then LogF("L4|" .. t.name .. "|nomethod|MISSING|")',
    '                elseif t.static then',
    '                    Case(t.name, "static-x", true, f, unpack(xs))',
    '                    Case(t.name, "static-table", true, f, {})',
    '                    Case(t.name, "static-nil", true, f, nil)',
    '                else',
    '                    local other = (o == p) and capital or p',
    '                    Case(t.name, "noself-x", true, f, unpack(xs))',
    '                    Case(t.name, "nilself-x", true, f, nil, unpack(xs))',
    '                    Case(t.name, "stringself-x", true, f, "x", unpack(xs))',
    '                    Case(t.name, "numberself-x", true, f, 12345, unpack(xs))',
    '                    Case(t.name, "tableself-x", true, f, {}, unpack(xs))',
    '                    Case(t.name, "otherobj-x", true, f, other, unpack(xs))',
    '                    Case(t.name, "noargs", true, f, o)',
    '                    Case(t.name, "badargs-x", true, f, o, unpack(xs))',
    '                    Case(t.name, "badargs-table", true, f, o, {})',
    '                end',
    '            end',
    '        end',
    '    end',
    '    -- section B: value-hostile calls on the read-only getters (oracle functions). The engine may fault on out-of-range ids: the SEH guard must turn that into an error.',
    '    local VALUES = { { "m1", -1 }, { "max", 2147483647 }, { "min", -2147483648 }, { "big", 10000000000 }, { "half", 0.5 }, { "huge", 1e300 }, { "nan", 0/0 } }',
    '    for _, t in ipairs(LEVEL4) do',
    '        if t.getter and t.nargs > 0 and not t.static then',
    '            local ok0, o = pcall(t.reach, p, capital, unit)',
    '            if ok0 and o ~= nil then',
    '                LogF("L4|BEGIN|" .. t.name .. "|values")',
    '                local f = o[t.meth]',
    '                for _, v in ipairs(VALUES) do',
    '                    local args = {}',
    '                    for i = 1, t.nargs do args[i] = v[2] end',
    '                    Case(t.name, "value-" .. v[1], false, f, o, unpack(args))',
    '                end',
    '            end',
    '        end',
    '    end',
    '    -- section C: functions WITHOUT a guard (no arguments, or bools only): invalid self values only. If GetInstance wrongly accepted one, the function would run natively.',
    '    for _, t in ipairs(LEVEL4) do',
    '        if not t.guarded and not t.static then',
    '            local ok0, o = pcall(t.reach, p, capital, unit)',
    '            if ok0 and o ~= nil then',
    '                LogF("L4|BEGIN|" .. t.name .. "|badself")',
    '                local f = o[t.meth]',
    '                local other = (o == p) and capital or p',
    '                Case(t.name, "noself", true, f)',
    '                Case(t.name, "nilself", true, f, nil)',
    '                Case(t.name, "stringself", true, f, "x")',
    '                Case(t.name, "numberself", true, f, 12345)',
    '                Case(t.name, "tableself", true, f, {})',
    '                Case(t.name, "otherobj", true, f, other)',
    '            end',
    '        end',
    '    end',
    '    LogF("L4|END|err=" .. stat.err .. "|ok=" .. stat.ok .. "|unexpected_ok=" .. stat.unexpected .. "|fault=" .. stat.fault)',
    'end', '']

os.makedirs(os.path.join(MOD, 'Scripts'), exist_ok=True)
shutil.copy(os.path.join(DEV, 'tools', 'CE_Proc.lua'), os.path.join(MOD, 'Scripts', 'CE_Proc.lua'))   # processor-fix test (CE issue #5)
os.makedirs(os.path.join(MOD, 'Data'), exist_ok=True)
os.makedirs(os.path.join(MOD, 'Binaries', 'Win64'), exist_ok=True)

L = ['-- SPDX-License-Identifier: AGPL-3.0-only', '-- Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W. Copyright (C) 2026 cru121. Licensed under the GNU AGPL v3.0 (see LICENSE.txt).',
     '-- GENERATED by dev-ce/tools/gen_selftest.py. Level 1 self-test of the Dev CE native bridge (presence of the generated methods).',
     '-- Output goes to Lua.log; search for "DevCE".', '',
     'local function Log(msg) print("DevCE: " .. msg) end',
     '-- Game and Map are static tables (Game.Foo(x), no object): wrap them so the generated tests can use the o:Method(x) form for every interface.',
     'local function DevStatic(t) return setmetatable({}, { __index = function(_, k) local f = t[k]; return function(_, ...) return f(...) end end }) end', '',
     'local EXPECT = {']
for itf, es in sorted(by.items()):
    names = ', '.join('"%s"' % e['lua'].split('.', 1)[1] for e in es)
    L.append('    { iface = "%s", object = "%s", reach = %s, methods = { %s } },' % (
        itf, es[0]['lua'].split('.', 1)[0], 'function(p, capital, unit) return %s end' % re.sub(r'^DevStatic\((\w+)\)$', r'\g<1>', ACCESS[itf]) if itf in ACCESS else 'nil', names))
L += ['}', '',
      'local function Methods(o)',
      '    local mt = getmetatable(o)',
      '    local idx = mt and mt.__index',
      '    local set = {}',
      '    if type(idx) == "table" then for k, v in pairs(idx) do set[tostring(k)] = true end end',
      '    return set',
      'end', '',
      'local done = false',
      'local function Run()',
      '    local p = Players[0]',
      '    local capital, unit',
      '    pcall(function() capital = p:GetCities():GetCapitalCity() end)',
      '    pcall(function() for _, u in p:GetUnits():Members() do unit = u break end end)',
      '    local present, missing, unreached = 0, 0, 0',
      '    for _, e in ipairs(EXPECT) do',
      '        local inst',
      '        if e.reach then',
      '            local ok, o = pcall(e.reach, p, capital, unit)',
      '            if ok then inst = o end',
      '        end',
      '        if inst == nil then',
      '            unreached = unreached + #e.methods',
      '            Log("NOT REACHED " .. e.iface .. " (" .. #e.methods .. " methods): no live instance")',
      '        else',
      '            local have = Methods(inst)',
      '            for _, m in ipairs(e.methods) do',
      '                if have[m] then present = present + 1 else missing = missing + 1; Log("MISSING " .. e.object .. ":" .. m .. " (" .. e.iface .. ")") end',
      '            end',
      '        end',
      '    end',
      '    Log(string.format("SELFTEST level 1: %d methods present, %d missing, %d not reached", present, missing, unreached))',
      'end', '',
      ] + L2LINES + ORACLE_LINES + L3LINES + L4LINES + [
      'local tries = 0',
      'GameEvents.PlayerTurnStarted.Add(function(playerID)',
      '    if DevCE_RecordState then DevCE_RecordState() end   -- lets the Frida test tool find the gameplay lua_State',
      '    if playerID == 0 and DevCE_IsArmed and DevCE_IsArmed() == 4 then',
      '        local ok4, err4 = pcall(Level3)',
      '        if not ok4 then Log("LEVEL3 error: " .. tostring(err4)) end',
      '    end',
      '    if playerID == 0 and DevCE_IsArmed and DevCE_IsArmed() == 6 then',
      '        local ok6, err6 = pcall(Level4)',
      '        if not ok6 then Log("LEVEL4 error: " .. tostring(err6)) end',
      '    end',
      '    if playerID == 0 and DevCE_IsArmed and DevCE_IsArmed() == 1 then',
      '        local ok2, err2 = pcall(Level2)',
      '        if not ok2 then Log("LEVEL2 error: " .. tostring(err2)) end',
      '    end',
      '    if done or playerID ~= 0 then return end',
      '    tries = tries + 1',
      '    local hasCity = false',
      '    pcall(function() hasCity = Players[0]:GetCities():GetCapitalCity() ~= nil end)',
      '    if not hasCity and tries < 12 then return end    -- wait until a city and a unit exist (found the capital, then end turn)',
      '    done = true',
      '    local ok, err = pcall(Run)',
      '    local ok3, err3 = pcall(Oracle)',
      '    if not ok3 then Log("ORACLE error: " .. tostring(err3)) end',
      '    if not ok then Log("SELFTEST error: " .. tostring(err)) end',
      'end)',
      'Log("self-test loaded; it runs at the first turn start after you founded a city (or at turn 12)")']
open(os.path.join(MOD, 'Scripts', 'DevCE_Selftest.lua'), 'w', encoding='utf-8').write('\n'.join(L) + '\n')

open(os.path.join(MOD, 'Data', 'Config.sql'), 'w', encoding='utf-8').write(
    "UPDATE GameCores\nSET\n    PackageId = '%s',\n    DllPrefix = 'GameCore_XP2_CE'\nWHERE\n    GameCore = 'Expansion2';\n" % GUID)

open(os.path.join(MOD, 'DevCE_Test.modinfo'), 'w', encoding='utf-8').write('''<?xml version="1.0" encoding="utf-8"?>
<Mod id="%s" version="1">
    <Properties>
        <Name>Dev CE test build</Name>
        <Description>EXPERIMENTAL. Dev CE with its automatic self-tests: a fork of the Community Extension GameCore that adds %d engine functions to existing Lua objects, plus scripts that test them at the start of your first turns (results in Lua.log lines containing "DevCE" and in DevBridge.log next to the DLL). SINGLE PLAYER ONLY, may desync multiplayer. Works only with Steam build 15038592 (disables itself otherwise). Incompatible with every other GameCore mod, including the Community Extension. Known unsafe: Unit.ChangeSightRange (hung the game once), PlayerTrade.Change*TradeDisabledCount (routes are not restored). Report problems with DevBridge.log.</Description>
        <Teaser>Dev CE native bridge with self-tests (experimental)</Teaser>
        <Authors>cru121, based on the Community Extension by Wild-W</Authors>
        <CompatibleVersions>1.2,2.0</CompatibleVersions>
        <AffectsSavedGames>0</AffectsSavedGames>
    </Properties>
    <Dependencies>
        <Mod id="4873eb62-8ccc-4574-b784-dda455e74e68" title="Expansion: Gathering Storm" />
    </Dependencies>
    <FrontEndActions>
        <UpdateDatabase id="DevCE_Config">
            <Properties><LoadOrder>10</LoadOrder></Properties>
            <File>Data/Config.sql</File>
        </UpdateDatabase>
    </FrontEndActions>
    <InGameActions>
        <AddGameplayScripts id="DevCE_Selftest">
            <Properties><LoadOrder>10</LoadOrder></Properties>
            <File>Scripts/DevCE_Selftest.lua</File>
        </AddGameplayScripts>
        <AddGameplayScripts id="DevCE_ProcTest">
            <Properties><LoadOrder>11</LoadOrder></Properties>
            <File>Scripts/CE_Proc.lua</File>
        </AddGameplayScripts>
    </InGameActions>
    <Files>
        <File>Binaries/Win64/GameCore_XP2_CE_FinalRelease.dll</File>
        <File>Data/Config.sql</File>
        <File>Scripts/DevCE_Selftest.lua</File>
        <File>Scripts/CE_Proc.lua</File>
    </Files>
</Mod>
''' % (GUID, len(man)))

# The player mod: same DLL, no scripts (nothing runs by itself). Different id, so it can be told apart from the test mod; enable only one of the two.
PLAYER = os.path.join(DEV, 'mod', 'DevCE')
PLAYER_GUID = '2f6d8f0e-5b9a-4a3e-8c55-7d2b6e1c9a40'
os.makedirs(os.path.join(PLAYER, 'Data'), exist_ok=True)
os.makedirs(os.path.join(PLAYER, 'Binaries', 'Win64'), exist_ok=True)
open(os.path.join(PLAYER, 'Data', 'Config.sql'), 'w', encoding='utf-8').write(
    "UPDATE GameCores\nSET\n    PackageId = '%s',\n    DllPrefix = 'GameCore_XP2_CE'\nWHERE\n    GameCore = 'Expansion2';\n" % PLAYER_GUID)
open(os.path.join(PLAYER, 'DevCE.modinfo'), 'w', encoding='utf-8').write('''<?xml version="1.0" encoding="utf-8"?>
<Mod id="%s" version="1">
    <Properties>
        <Name>Dev CE (experimental)</Name>
        <Description>EXPERIMENTAL. A fork of the Community Extension GameCore that adds %d engine functions to existing Lua objects (City, Unit, Player, Game, Map, ...). Nothing runs by itself; call the new methods from your own scripts. SINGLE PLAYER ONLY, may desync multiplayer; ids and indices you pass are not range-checked. Works only with Steam build 15038592 (disables itself otherwise). Incompatible with every other GameCore mod, including the Community Extension. Known unsafe: Unit.ChangeSightRange, PlayerTrade.Change*TradeDisabledCount. DevBridge.log next to the DLL describes what loaded; attach it to bug reports.</Description>
        <Teaser>Engine functions for Lua modders (experimental)</Teaser>
        <Authors>cru121, based on the Community Extension by Wild-W</Authors>
        <CompatibleVersions>1.2,2.0</CompatibleVersions>
        <AffectsSavedGames>0</AffectsSavedGames>
    </Properties>
    <Dependencies>
        <Mod id="4873eb62-8ccc-4574-b784-dda455e74e68" title="Expansion: Gathering Storm" />
    </Dependencies>
    <FrontEndActions>
        <UpdateDatabase id="DevCE_Config">
            <Properties><LoadOrder>10</LoadOrder></Properties>
            <File>Data/Config.sql</File>
        </UpdateDatabase>
    </FrontEndActions>
    <InGameActions>
        <AddGameplayScripts id="DevCE_Loaded">
            <Properties><LoadOrder>10</LoadOrder></Properties>
            <File>Scripts/DevCE_Loaded.lua</File>
        </AddGameplayScripts>
    </InGameActions>
    <Files>
        <File>Binaries/Win64/GameCore_XP2_CE_FinalRelease.dll</File>
        <File>Data/Config.sql</File>
        <File>Scripts/DevCE_Loaded.lua</File>
    </Files>
</Mod>
''' % (PLAYER_GUID, len(man)))
# A mod without any in-game action is treated by the game as front-end only and its GameCore is NOT used (found 2026-10-06: the first player mod loaded the vanilla DLL).
# So the player mod carries one in-game script that only writes a line to Lua.log.
os.makedirs(os.path.join(PLAYER, 'Scripts'), exist_ok=True)
open(os.path.join(PLAYER, 'Scripts', 'DevCE_Loaded.lua'), 'w', encoding='utf-8').write('''-- SPDX-License-Identifier: AGPL-3.0-only
-- Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W. Copyright (C) 2026 cru121. Licensed under the GNU AGPL v3.0 (see LICENSE.txt).
-- Dev CE (experimental): this script only writes one line to Lua.log at the start of the first turn. (A mod needs at least one in-game action,
-- otherwise the game treats it as a front-end mod and does not use its GameCore DLL.)
local done = false
GameEvents.PlayerTurnStarted.Add(function(playerID)
    if done then return end
    done = true
    local present = false
    pcall(function() present = (Game.GetEras().SetCurrentEra ~= nil) end)
    print("DevCE: player mod loaded; Dev CE methods " .. (present and "PRESENT" or "MISSING (the Dev CE GameCore was not loaded)"))
end)
''')

dll = os.path.join(DEV, 'build', 'GameCore_XP2_CE_FinalRelease.dll')
if os.path.exists(dll):
    shutil.copy(dll, os.path.join(MOD, 'Binaries', 'Win64', 'GameCore_XP2_CE_FinalRelease.dll'))
    shutil.copy(dll, os.path.join(PLAYER, 'Binaries', 'Win64', 'GameCore_XP2_CE_FinalRelease.dll'))
print('mod written to', MOD, '| methods', len(man), '| interfaces', len(by), '| reachable interfaces', sum(1 for i in by if i in ACCESS))
