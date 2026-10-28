// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#pragma once
#include "Data.h"
#include <string>

// Calls the game's own processor dispatcher (GameCore::Lua::Utility::CallProcessor) instead of running Lua from CE.
//
// The game runs `GameEvents[name].Call(<args>, map)` on its single script-system Lua state, releasing the GameCore lock around the call.
// Observed in game (see findings/topics/lua-event-dispatch.md): all gameplay events, including the Congress processors, are dispatched
// from the simulation thread with the GameCore lock held. Call() therefore refuses to run when the calling thread does not hold that lock.
namespace GameProcessor {
	// Looks up the addresses. Call once at startup, after Runtime::Create().
	extern void Create();

	// Sends `variantMap` to the processor `name` as the table argument of GameEvents[name].Call(table).
	// Only integer entries are transferred (and read back); other entries are ignored.
	// Returns true when a handler ran and returned a non-nil value (the game's own meaning of "handled"); the integers are then updated from the table.
	// Returns false when nothing handled it, when the game state is not ready, or when the calling thread does not hold the GameCore lock.
	extern bool Call(const std::string& name, Data::LuaVariantMap& variantMap, bool requireGameCoreLock = true);
	// requireGameCoreLock = false is for callers that run inside a Lua handler: the game releases the lock around every Lua call (Lua::Utility::CallHook/CallProcessor),
	// so a handler runs on the simulation thread WITHOUT the lock, and calling the dispatcher from there is what GameEvents[name].Call(...) does too.

	// True when the calling thread holds the GameCore lock (IEngineUtility::HasGameCoreLock).
	extern bool HasGameCoreLock();
	// Polls the game's own test dispatcher (GameCore::Lua::Utility::CallTestAll): runs GameEvents[name].TestAll(<int args>) on the script-system Lua state.
	// Returns true when a handler ran; `result` is then the combined answer (false as soon as one handler returned false). Same rule the game uses for
	// GameEvents.CanStartOperation: veto only when the return value is true AND result is false. Safe to call from any thread the game's own dispatch is called from
	// (the game's wrapper releases and re-takes the GameCore lock itself). Returns false without touching `result` before Create() or when the script system is not up yet.
	extern bool TestAll(const char* name, const int* args, int count, bool& result);

}
