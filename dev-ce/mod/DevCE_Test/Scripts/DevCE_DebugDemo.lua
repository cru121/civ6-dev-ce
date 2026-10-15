-- DevCE_DebugDemo.lua: target for the Dev CE socket debugger (dev-ce/tools/devce_dbg.py). Does nothing unless the debugger server is running
-- (marker file DevCE_Debug.txt next to the DLL). Waits up to 60 s for a client, then runs a small call chain that stops at debug.c_breakpoint().
if DevCE_DebugActive and DevCE_DebugActive() == 1 then
    DemoMarker = "hello from the demo"
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
    if DevCE_DebugWait(60) == 1 then
        DemoResult = outer()
        if DevCE_Log then DevCE_Log("DEBUGDEMO|result " .. tostring(DemoResult)) end
    elseif DevCE_Log then
        DevCE_Log("DEBUGDEMO|no client connected within 60 s")
    end
end
