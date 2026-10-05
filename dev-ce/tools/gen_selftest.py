#!/usr/bin/env python3
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
for e in man:
    if e.get('oracle') or e['iface'] not in ACCESS:
        continue
    fn = e['lua'].split('.', 1)[1]
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

os.makedirs(os.path.join(MOD, 'Scripts'), exist_ok=True)
os.makedirs(os.path.join(MOD, 'Data'), exist_ok=True)
os.makedirs(os.path.join(MOD, 'Binaries', 'Win64'), exist_ok=True)

L = ['-- GENERATED by dev-ce/tools/gen_selftest.py. Level 1 self-test of the Dev CE native bridge (presence of the generated methods).',
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
      ] + L2LINES + ORACLE_LINES + L3LINES + [
      'local tries = 0',
      'GameEvents.PlayerTurnStarted.Add(function(playerID)',
      '    if DevCE_RecordState then DevCE_RecordState() end   -- lets the Frida test tool find the gameplay lua_State',
      '    if playerID == 0 and DevCE_IsArmed and DevCE_IsArmed() == 4 then',
      '        local ok4, err4 = pcall(Level3)',
      '        if not ok4 then Log("LEVEL3 error: " .. tostring(err4)) end',
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
        <Description>Development build of a Community-Extension-style GameCore with generated native bridge methods (%d). Single-player testing only. Incompatible with other GameCore mods (including the Community Extension).</Description>
        <Teaser>Dev CE native bridge test</Teaser>
        <Authors>local test</Authors>
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
    </InGameActions>
    <Files>
        <File>Binaries/Win64/GameCore_XP2_CE_FinalRelease.dll</File>
        <File>Data/Config.sql</File>
        <File>Scripts/DevCE_Selftest.lua</File>
    </Files>
</Mod>
''' % (GUID, len(man)))

dll = os.path.join(DEV, 'build', 'GameCore_XP2_CE_FinalRelease.dll')
if os.path.exists(dll):
    shutil.copy(dll, os.path.join(MOD, 'Binaries', 'Win64', 'GameCore_XP2_CE_FinalRelease.dll'))
print('mod written to', MOD, '| methods', len(man), '| interfaces', len(by), '| reachable interfaces', sum(1 for i in by if i in ACCESS))
