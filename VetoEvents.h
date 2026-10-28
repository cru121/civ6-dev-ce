// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#pragma once

// Lua veto events for city operations and city commands.
//
// The game polls GameEvents.CanStartOperation (GameCore::Lua::Utility::CallTestAll) before a UNIT operation starts; a handler returning false blocks it.
// City operations and city commands have no such call. This module adds it with two detours:
//   GameEvents.CanStartCityOperation(playerId, cityId, operationType)   hooks City::Operations::Manager::CanStart
//   GameEvents.CanStartCityCommand(playerId, cityId, commandType)       hooks City::Commands::Manager::CanStart
// Both run after the original check said yes; any handler returning false turns the answer into false. With no handler registered nothing changes.
// Numbers: the only city operation is BUILD (0). Command types are the CityCommandTypes ordinals: 0 PURCHASE and 1 RANGE_ATTACK are verified in game,
// the rest (2 SET_FOCUS, 3 DESTROY, 4 MANAGE, 5 WMD_STRIKE, 6 SWAP_TILE_OWNER, 7 NAME_CITY) are inferred. See findings/topics/lua-event-dispatch.md.
namespace VetoEvents {
	// Installs the two detours. Call once at startup, after Runtime::Create() and GameProcessor::Create().
	extern void Create();
}
