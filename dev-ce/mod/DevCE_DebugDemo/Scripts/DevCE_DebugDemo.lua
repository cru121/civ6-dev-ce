-- DevCE_DebugDemo.lua: target for the Dev CE socket debugger (dev-ce/tools/devce_dbg.py). Does nothing unless the marker file has the line "demo" (DevCE_DebugDemo() == 1)
-- (second line of DevCE_Debug.txt next to the DLL). Waits up to 60 s for a client, then runs: a call chain that stops at debug.c_breakpoint(), probe()
-- (locals, an upvalue and nested tables to inspect), a loop calling work() three times, and a busy loop of up to 25 s (target for `pause`).
if DevCE_DebugDemo and DevCE_DebugDemo() == 1 then
    DemoMarker = "hello from the demo"
    local upv = 100
    local function inner(a, b)
        local sum = a + b
        debug.c_breakpoint()
        sum = sum * 2
        return sum
    end
    local function outer()
        local r = inner(20, 22)
        r = r + 1
        return r
    end
    local function probe(a, b)
        local t = { 10, 20, x = 1, y = { z = 2 }, name = "tbl" }
        local s = "str"
        local n = a + b + upv
        DemoProbe = n
        return n
    end
    local function work(i)
        DemoI = i
        local a = i * 2
        return a + 1
    end
    if DevCE_DebugWait(60) == 1 then
        DemoResult = outer()
        DemoProbeResult = probe(1, 2)
        DemoSum = 0
        for i = 1, 3 do DemoSum = DemoSum + work(i) end
        local t = os.clock()
        DemoSpin = true
        while DemoSpin and os.clock() - t < 25 do end
        if DevCE_Log then DevCE_Log("DEBUGDEMO|result " .. tostring(DemoResult) .. " probe " .. tostring(DemoProbeResult) .. " sum " .. tostring(DemoSum) .. " spin " .. tostring(DemoSpin)) end
    elseif DevCE_Log then
        DevCE_Log("DEBUGDEMO|no client connected within 60 s")
    end
end
