-- DevCE_TurnTimer.lua: marks the start of every game turn in DevBridge.log (and for a connected debugger client) together with the Lua slow-path counters (D4 measurements).
if DevCE_DebugMark and GameEvents and GameEvents.OnGameTurnStarted then
    GameEvents.OnGameTurnStarted.Add(function(turn) DevCE_DebugMark("game_turn " .. tostring(turn)) end)
end
