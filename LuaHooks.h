// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#pragma once
#include "HavokScript.h"

// Working Lua hooks for the retail HavokScript VM: debug.sethook / debug.gethook with call, return, line and count events.
// The retail interpreter never reads the hook fields that lua_sethook stores in the lua_State, so we patch the VM's per-instruction fetch site
// (HavokScript_FinalRelease.dll, only builds 15038592 / 15296837, byte-pattern checked) on the first debug.sethook call. Design notes and the
// measurements behind it: dev-ce/DESIGN_LUA_HOOKS.md (M0 results).
namespace LuaHooks {
	// Adds debug.sethook / debug.gethook to the global `debug` table of this state (creating the table with HavokScript's own luaopen_debug if it is missing).
	void RegisterGlobals(hks::lua_State* L);

	// True once the VM patch is installed (first successful debug.sethook with a non-zero mask).
	bool IsInstalled();
}
