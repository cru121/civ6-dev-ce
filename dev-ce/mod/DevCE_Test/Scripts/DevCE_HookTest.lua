-- DevCE_HookTest.lua: self-test of Dev CE's debug.sethook / debug.gethook (native Lua hooks, LuaHooks.cpp).
-- Runs once when the gameplay script loads. Results: Lua.log lines "DevCE HookTest: PASS/FAIL ..." and DevBridge.log lines "HOOKTEST|...".
-- The first debug.sethook with a non-zero mask installs the VM patch (see DevBridge.log "[LuaHooks] fetch site patched").
local passed, failed = 0, 0

local function log(s)
    print("DevCE HookTest: " .. s)
    if DevCE_Log then DevCE_Log("HOOKTEST|" .. s) end
end

local function check(name, ok, detail)
    if ok then passed = passed + 1 else failed = failed + 1 end
    log((ok and "PASS " or "FAIL ") .. name .. ((detail ~= nil) and ("  [" .. tostring(detail) .. "]") or ""))
end

local function now() return DevCE_ClockMs and DevCE_ClockMs() or (os.clock() * 1000) end

local function timedLoop(n)
    local t0 = now()
    local x = 0
    for i = 1, n do x = x + i end
    return now() - t0, x
end

local function runTests()
    if not (debug and debug.sethook and debug.gethook) then
        log("FAIL debug.sethook / debug.gethook are missing (native hooks not registered)")
        failed = failed + 1
        return
    end

    -- baseline speed before the VM patch exists (os.clock may have a coarse resolution)
    local base = timedLoop(3000000)

    -- (0) no hook set
    local h, m, c = debug.gethook()
    check("gethook with no hook", h == nil and m == "" and c == 0, tostring(h) .. "," .. tostring(m) .. "," .. tostring(c))

    -- (a) line hook: a function of four lines must produce four consecutive line events, and the line passed to the hook must equal currentline of level 2
    local lines, levelHits, events = {}, { [0] = 0, 0, 0, 0 }, 0
    local function target()
        local a = 1
        local b = 2
        local c = a + b
        return c
    end
    local function lineHook(ev, line)
        lines[#lines + 1] = line
        events = events + 1
        if debug.getinfo then
            for k = 0, 3 do
                local info = debug.getinfo(k, "l")
                if info and info.currentline == line then levelHits[k] = levelHits[k] + 1 end
            end
        end
    end
    debug.sethook(lineHook, "l")
    local r = target()
    debug.sethook()
    local run = 0
    for i = 2, #lines do
        if lines[i] == lines[i - 1] + 1 then run = run + 1; if run >= 3 then break end else run = 0 end
    end
    check("line hook: target() result", r == 3, r)
    check("line hook: four consecutive lines seen", run >= 3, table.concat(lines, ","))
    -- inside the hook, PUC Lua numbers the hooked function as level 2 (1 = the hook function itself); report which level Havok's getinfo agrees with
    local match = nil
    for k = 0, 3 do if levelHits[k] == events then match = k end end
    log(string.format("INFO getinfo level whose currentline equals the line argument for all %d events: %s (hits per level 0..3: %d,%d,%d,%d)",
        events, tostring(match), levelHits[0], levelHits[1], levelHits[2], levelHits[3]))
    check("line hook: getinfo(2).currentline equals the line argument (PUC numbering)", levelHits[2] == events, match)

    -- (a2) a loop on a single line: PUC Lua fires a line event per iteration (the pc jumps backwards)
    local lc = 0
    debug.sethook(function() lc = lc + 1 end, "l")
    for i = 1, 5 do local z = i end
    debug.sethook()
    check("line hook: one event per iteration of a single-line loop", lc >= 5 and lc <= 9, lc)

    -- (h) gethook after sethook
    local f = function() end
    debug.sethook(f, "lcr", 0)
    local gh, gm, gc = debug.gethook()
    check("gethook returns hook, mask, count", gh == f and gm == "crl" and gc == 0, tostring(gm) .. "," .. tostring(gc))
    debug.sethook(f, "l", 7)
    local _, _, gc2 = debug.gethook()
    check("gethook count", gc2 == 7, gc2)
    debug.sethook()
    local h2 = debug.gethook()
    check("hook removed by sethook()", h2 == nil)

    -- (b) count hook
    local n = 0
    debug.sethook(function(ev) if ev == "count" then n = n + 1 end end, "", 100)
    local x = 0
    for i = 1, 1000 do x = x + i end
    debug.sethook()
    check("count hook: loop result intact", x == 500500, x)
    check("count hook: fired about once per 100 instructions", n >= 10 and n <= 120, n)

    -- (c) call / return hook
    local evs = {}
    local function leaf() return 1 end
    local function mid() return leaf() + 1 end
    debug.sethook(function(e) evs[#evs + 1] = e end, "cr")
    local v = mid()
    debug.sethook()
    local want, wi = { "call", "call", "return", "return" }, 1
    for i = 1, #evs do if evs[i] == want[wi] then wi = wi + 1; if wi > #want then break end end end
    check("call/return hook: mid() result", v == 2, v)
    check("call/return hook: call,call,return,return seen in order", wi > #want, table.concat(evs, ","))

    -- (d) a hook that calls deeply recursive Lua code (grows and moves the Lua stack): hooked code must still compute correctly
    local function deep(k) if k == 0 then return 0 end return 1 + deep(k - 1) end
    local calls, deepv = 0, nil
    debug.sethook(function() calls = calls + 1; if calls == 3 then deepv = deep(500) end end, "l")
    local s = 0
    for i = 1, 50 do s = s + i end
    local t = { 1, 2, 3 }
    local u = t[1] + t[2] + t[3]
    debug.sethook()
    check("hook that grows the Lua stack: hooked code result", s == 1275 and u == 6, tostring(s) .. "," .. tostring(u))
    check("hook that grows the Lua stack: deep recursion inside the hook", deepv == 500, deepv)

    -- (e) an error inside the hook must not crash; Dev CE removes the hook (PUC would propagate the error)
    debug.sethook(function() error("boom") end, "l")
    local acc = 0
    for i = 1, 5 do acc = acc + i end
    local after = debug.gethook()
    check("error in hook: code continues", acc == 15, acc)
    check("error in hook: hook removed", after == nil)

    -- (i) coroutines created while a hook mask is set
    debug.sethook(function() end, "l")
    local co = coroutine.create(function() local z = 0 for i = 1, 100 do z = z + i end return z end)
    local ok, val = coroutine.resume(co)
    debug.sethook()
    check("coroutine with an inherited hook mask", ok and val == 5050, tostring(ok) .. "," .. tostring(val))

    -- (k) debug.sethook(thread, ...) / debug.gethook(thread): hook a suspended coroutine from outside; the main thread must see no events
    local mainEv, coEv = 0, 0
    local co2 = coroutine.create(function() local z = 0 for i = 1, 20 do z = z + i end coroutine.yield(z) return z end)
    debug.sethook(co2, function() coEv = coEv + 1 end, "l")
    local gh2, gm2 = debug.gethook(co2)
    local mh = debug.gethook()
    local ok2, v2 = coroutine.resume(co2)
    debug.sethook(co2)
    local gh3 = debug.gethook(co2)
    check("thread argument: gethook(thread) sees the hook, the running state does not", type(gh2) == "function" and gm2 == "l" and mh == nil, tostring(gm2))
    check("thread argument: coroutine hooked from outside produced line events", ok2 and v2 == 210 and coEv >= 3, tostring(v2) .. "," .. coEv)
    check("thread argument: hook removed with sethook(thread)", gh3 == nil)

    -- (j) cost: loop of 3M iterations before the patch existed, with the patch but no hook, with a count hook, with a line hook (informational)
    local noHook, xx = timedLoop(3000000)
    check("loop result with the patch installed", xx == 4500001500000, xx)
    local cnt = 0
    debug.sethook(function() cnt = cnt + 1 end, "", 1000)
    local withCount = timedLoop(3000000)
    debug.sethook()
    local lcnt = 0
    debug.sethook(function() lcnt = lcnt + 1 end, "l")
    local withLine = timedLoop(100000)
    debug.sethook()
    log(string.format("INFO 3M-iteration loop in ms: before first sethook %.1f, patch installed no hook %.1f, count hook every 1000 instructions %.1f (%d events), 100k iterations with a line hook %.1f (%d events)",
        base, noHook, withCount, cnt, withLine, lcnt))
end

local ok, err = pcall(runTests)
if not ok then
    failed = failed + 1
    log("FAIL test aborted with a Lua error: " .. tostring(err))
end
log(string.format("DONE passed=%d failed=%d", passed, failed))
