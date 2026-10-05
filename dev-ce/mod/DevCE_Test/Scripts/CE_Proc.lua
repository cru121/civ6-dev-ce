-- SPDX-License-Identifier: AGPL-3.0-only
-- Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
-- Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
-- Prototype test for Community Extension issue #5 (processors through the game's own dispatcher).
-- Output goes to Lua.log (search for "CE_Proc").

local function Log(msg)
    print("CE_Proc: " .. msg)
end

if RegisterProcessor == nil or ProcessorTest == nil then
    Log("RegisterProcessor / ProcessorTest are NOT available - the custom GameCore did not load.")
    return
end
Log("RegisterProcessor and ProcessorTest are available.")

-- 1. A processor that changes the value it receives. Native side sends {Value = n}; the handler returns true and writes n + 1 back.
RegisterProcessor("CE_Proc_Test", function(map)
    Log("handler CE_Proc_Test called, map.Value = " .. tostring(map.Value))
    map.Value = map.Value + 1
    return true
end)

-- 2. A processor whose handler returns nothing. The game's dispatcher only reports that a handler RAN, so the native caller sees handled = true here
--    (the first prototype run showed this); a handler that wants to claim a decision must say so in the map (AI.cpp uses map.Handled = 1).
RegisterProcessor("CE_Proc_Declined", function(map)
    Log("handler CE_Proc_Declined called, map.Value = " .. tostring(map.Value))
    map.Value = 999
end)

-- 3. The congress target choosers CE hooks natively (AI.cpp). Only logging; not setting map.Handled makes CE fall back to the game's own chooser.
for _, name in ipairs({"DistrictTargetChooser", "UnitPromotionClassTargetChooser", "UnitBuildYieldTargetChooser", "TradingPartnersTargetChooser",
                       "PlayerOrDiploLeaderTargetChooser", "GreatPersonClassTargetChooser", "GreatPersonPatronageTargetChooser",
                       "SpyOperationTargetChooser", "MostCommonLuxuryTargetChooser", "MinorCivBonusTargetChooser", "GrievancesTypeTargetChooser"}) do
    local count = 0
    RegisterProcessor(name, function(map)
        count = count + 1
        if count <= 3 then
            Log(name .. " called (#" .. count .. "): OutcomeType=" .. tostring(map.OutcomeType) .. " PlayerId=" .. tostring(map.PlayerId))
        end
    end)
end

-- Run the native-path tests at the start of the first three turns of the human player (this runs on the simulation thread with the GameCore lock held).
local turnsTested = 0
local function OnPlayerTurnStarted(playerID)
    local player = Players[playerID]
    if player == nil or not player:IsHuman() or turnsTested >= 3 then return end
    turnsTested = turnsTested + 1

    local handled, value = ProcessorTest("CE_Proc_Test", "Value", 41)
    Log("test 1 (handler adds 1): handled=" .. tostring(handled) .. " value=" .. tostring(value) .. "  expected handled=true value=42")

    handled, value = ProcessorTest("CE_Proc_Declined", "Value", 5)
    Log("test 2 (handler returns nothing): handled=" .. tostring(handled) .. " value=" .. tostring(value) .. "  expected handled=true value=999")

    handled, value = ProcessorTest("CE_Proc_NobodyRegistered", "Value", 7)
    Log("test 3 (no handler): handled=" .. tostring(handled) .. " value=" .. tostring(value) .. "  expected handled=false value=7")
end

GameEvents.PlayerTurnStarted.Add(OnPlayerTurnStarted)
