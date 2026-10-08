-- SPDX-License-Identifier: AGPL-3.0-only
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

    -- GetIO() test: the game does not open Lua's io library; Dev CE exposes it. Writes devce_io_test.txt (relative to the game's working directory) and reads it back.
    local ok, err = pcall(function()
        local io = GetIO()
        local f = assert(io.open("devce_io_test.txt", "w"))
        f:write("Hello from Dev CE")
        f:close()
        local r = assert(io.open("devce_io_test.txt", "r"))
        local text = r:read("*a")
        r:close()
        print("DevCE: GetIO test " .. (text == "Hello from Dev CE" and "PASS" or ("FAIL, read back: " .. tostring(text))))
    end)
    if not ok then print("DevCE: GetIO test ERROR: " .. tostring(err)) end
end)
