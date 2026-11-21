// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#pragma once
#include "HavokScript.h"

// Lua debugger over a loopback socket (D0 of dev-ce/DESIGN_LUA_DEBUGGER.md): stop in a state, look at the stack, evaluate expressions, continue / step.
// Off by default. Enabled by a file `DevCE_Debug.txt` next to the DLL (optional first line: TCP port, default 4320) or by DevCE_DebugListen(port) from Lua.
// Protocol: one JSON object per line over 127.0.0.1, see dev-ce/tools/devce_dbg.py.
namespace LuaDebugger {
	// Per Lua state: replaces debug.c_breakpoint (stops when a client is connected, otherwise a no-op), adds DevCE_DebugListen(port), DevCE_DebugWait(seconds),
	// DevCE_DebugActive(). Starts the server when the marker file exists. Call after LuaHooks::RegisterGlobals (needs the global `debug` table).
	void RegisterGlobals(hks::lua_State* L);
}
